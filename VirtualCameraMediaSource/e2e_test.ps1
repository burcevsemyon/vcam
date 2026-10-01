# E2E test: frame producer (VCamProducerCli) -> shared memory -> camera -> CaptureTest.
# Phases:
#   A) run --type video --path test_video.mp4 (CLI overrides) -> moving frames
#      + status "frames are being written", BMP must be 640x480 with letterbox
#   B) run (no overrides) + settings.json writes -> hot-switch static -> video
#   C) list-devices + run --type camera --device <id> (+ negative check with a
#      nonexistent id -> NO SIGNAL). No camera device -> SKIP, exit stays 0.
#   D/E) device mode (FrameServer proxy path): 1280x720 then 640x480 RGB32 -
#      allocator Lock sizes, no "buffer too small", letterbox aspect, full-height
#      content. Skipped if the installed DLL != freshly built DLL.
#   F) producer stop -> frozen fallback frames -> restart -> moving again.
# Precondition: tray host + stray CLI stopped (FAIL otherwise). Direct mode is
# pointed at the freshly built DLL via per-user HKCU CLSID registration
# (non-elevated regsvr32 is a no-op); the key is restored/removed in `finally`.
# settings.json is backed up before phase B and restored byte-for-byte afterwards.
$ErrorActionPreference = "Stop"

$Root = $PSScriptRoot
$BuildDir = Join-Path $Root "build\x64\Release"
$MediaDll = Join-Path $BuildDir "MediaSource.dll"
$Cli = Join-Path $BuildDir "VCamProducerCli.exe"
$CaptureTest = Join-Path $BuildDir "CaptureTest.exe"
$OutDir = Join-Path $Root "e2e_output"
$TestVideo = Join-Path $OutDir "test_video.mp4"
$TestImage = Join-Path $Root "test_input.bmp"
$SettingsPath = Join-Path $env:APPDATA "VCam\settings.json"
$SettingsBackup = Join-Path $env:TEMP "vcam_e2e_settings_backup.json"
$Utf8NoBom = New-Object System.Text.UTF8Encoding($false)
$VCamClsid = "{B2B674D4-9CF0-461C-BDCE-3D56FBB41356}"
$ClsidKey = "HKCU:\Software\Classes\CLSID\$VCamClsid"
$InstalledDll = "C:\Program Files\VCam\MediaSource.dll"
$DiagLog = "C:\Users\Semen\AppData\Local\Temp\opencode\msrc_diag.log"

Add-Type -AssemblyName System.Drawing

$script:Failures = New-Object System.Collections.Generic.List[string]
function Fail([string]$msg) { $script:Failures.Add($msg); Write-Host "FAIL: $msg" -ForegroundColor Red }
function Pass([string]$msg) { Write-Host "PASS: $msg" -ForegroundColor Green }

function Write-TestSettings([string]$type) {
    $img = $TestImage -replace '\\', '\\'
    $vid = $TestVideo -replace '\\', '\\'
    $json = @"
{
  "source": { "type": "$type" },
  "static": { "path": "$img", "scaleMode": "fit", "cropX": 0, "cropY": 0, "cropW": 0, "cropH": 0, "cropKeepAspect": false },
  "video": { "path": "$vid" },
  "autostart": false
}
"@
    [IO.File]::WriteAllText($SettingsPath, $json, $Utf8NoBom)
}

function Capture([int]$n, [string]$prefix) {
    Remove-Item (Join-Path $OutDir ($prefix + "_*.bmp")) -ErrorAction SilentlyContinue
    & $CaptureTest $n (Join-Path $OutDir $prefix) 2>&1 |
        Out-File (Join-Path $OutDir ("capture_" + $prefix + ".log"))
    return @(Get-ChildItem (Join-Path $OutDir ($prefix + "_*.bmp")) -ErrorAction SilentlyContinue)
}

function Get-FrameInfo($file) {
    $bmp = New-Object System.Drawing.Bitmap $file.FullName
    $w = $bmp.Width
    $h = $bmp.Height
    $nonBlack = 0
    for ($x = 0; $x -lt $w; $x += 32) {
        for ($y = 0; $y -lt $h; $y += 18) {
            $c = $bmp.GetPixel($x, $y)
            if ($c.R -gt 10 -or $c.G -gt 10 -or $c.B -gt 10) { $nonBlack++ }
        }
    }
    $bmp.Dispose()
    [pscustomobject]@{
        Name     = $file.Name
        Width    = $w
        Height   = $h
        NonBlack = $nonBlack
        Hash     = (Get-FileHash $file.FullName -Algorithm SHA256).Hash
    }
}

# --- pixel helpers -----------------------------------------------------------
function Get-RowMax($bmp, [int]$y) {
    $m = 0
    for ($x = 0; $x -lt $bmp.Width; $x += 16) {
        $c = $bmp.GetPixel($x, $y)
        $v = [Math]::Max($c.R, [Math]::Max($c.G, $c.B))
        if ($v -gt $m) { $m = $v }
    }
    return $m
}

# 640x480 delivery must letterbox into the frame: black top/bottom bars,
# content centered (regression: "image stretched vertically").
# Returns $null when OK, otherwise the failure reason.
function Test-Letterbox640($file) {
    $bmp = New-Object System.Drawing.Bitmap $file.FullName
    try {
        if ($bmp.Width -ne 640 -or $bmp.Height -ne 480) {
            return "size $($bmp.Width)x$($bmp.Height) != 640x480 (SetMediaType NV12 640x480 regression?)"
        }
        foreach ($y in @(5, 25, 55)) {
            $m = Get-RowMax $bmp $y
            if ($m -ne 0) { return "top letterbox bar row $y not black (max=$m)" }
        }
        foreach ($y in @(425, 455, 475)) {
            $m = Get-RowMax $bmp $y
            if ($m -ne 0) { return "bottom letterbox bar row $y not black (max=$m)" }
        }
        return $null
    } finally { $bmp.Dispose() }
}

# Each listed row must contain at least one non-black sampled pixel.
function Test-RowsNonBlack($file, [int[]]$rows) {
    $bmp = New-Object System.Drawing.Bitmap $file.FullName
    try {
        foreach ($y in $rows) {
            if ((Get-RowMax $bmp $y) -le 10) { return "row $y is black (max<=10)" }
        }
        return $null
    } finally { $bmp.Dispose() }
}

# 1280x720 frames: content must reach top AND bottom of the frame
# (regression: "only the top part of the picture").
function Test-FullHeight720($file) {
    $bmp = New-Object System.Drawing.Bitmap $file.FullName
    try {
        if ($bmp.Width -ne 1280 -or $bmp.Height -ne 720) {
            return "size $($bmp.Width)x$($bmp.Height) != 1280x720"
        }
        foreach ($y in @(5, 714)) {
            $m = 0
            for ($x = 600; $x -le 680; $x += 8) {
                $c = $bmp.GetPixel($x, $y)
                $v = [Math]::Max($c.R, [Math]::Max($c.G, $c.B))
                if ($v -gt $m) { $m = $v }
            }
            if ($m -le 10) { return "center column row $y black (max=$m) - truncated frame?" }
        }
        return $null
    } finally { $bmp.Dispose() }
}

# Reads only the part of msrc_diag.log appended after $offset.
function Read-DiagWindow([long]$offset) {
    if (-not (Test-Path -LiteralPath $DiagLog)) { return "" }
    $fs = [IO.File]::Open($DiagLog, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite)
    try {
        if ($offset -gt $fs.Length) { $offset = 0 }
        $fs.Seek($offset, [IO.SeekOrigin]::Begin) | Out-Null
        $len = $fs.Length - $offset
        if ($len -le 0) { return "" }
        $buf = New-Object byte[] $len
        $read = 0
        while ($read -lt $len) {
            $n = $fs.Read($buf, $read, $len - $read)
            if ($n -le 0) { break }
            $read += $n
        }
        return [Text.Encoding]::UTF8.GetString($buf, 0, $read)
    } finally { $fs.Dispose() }
}

function Capture-Device([int]$idx, [int]$w, [int]$h, [string]$prefix) {
    Remove-Item (Join-Path $OutDir ($prefix + "_*.bmp")) -ErrorAction SilentlyContinue
    $logPath = Join-Path $OutDir ("capture_" + $prefix + ".log")
    $errPath = Join-Path $OutDir ("capture_" + $prefix + ".err")
    $p = Start-Process -FilePath $CaptureTest `
        -ArgumentList @("device", "$idx", "$w", "$h", (Join-Path $OutDir $prefix)) `
        -PassThru -WindowStyle Hidden -RedirectStandardOutput $logPath -RedirectStandardError $errPath -Wait
    $files = @(Get-ChildItem (Join-Path $OutDir ($prefix + "_*.bmp")) -ErrorAction SilentlyContinue)
    [pscustomobject]@{ ExitCode = $p.ExitCode; Files = $files; LogPath = $logPath }
}

# Finds the VCam device index in `CaptureTest inspect` output by capability:
# exactly 3 media types = RGB32 1280x720 + NV12 1280x720 + RGB32 640x480.
# (FRIENDLY_NAME is empty, so name filters fall back to device[0] = real camera.)
function Find-VCamDevice {
    $logPath = Join-Path $OutDir "inspect_vcam.log"
    $errPath = Join-Path $OutDir "inspect_vcam.err"
    $p = Start-Process -FilePath $CaptureTest -ArgumentList @("inspect") `
        -PassThru -WindowStyle Hidden -RedirectStandardOutput $logPath -RedirectStandardError $errPath -Wait
    if ($p.ExitCode -ne 0) { return $null }
    $idx = -1; $mt = -1; $has720 = $false; $has640 = $false
    $found = -1
    foreach ($line in @(Get-Content -LiteralPath $logPath)) {
        if ($line -match "device\[(\d+)\]") {
            if ($idx -ge 0 -and $mt -eq 3 -and $has720 -and $has640) { if ($found -lt 0) { $found = $idx } }
            $idx = [int]$Matches[1]; $mt = -1; $has720 = $false; $has640 = $false
        } elseif ($line -match "mediaTypes=(\d+)") { $mt = [int]$Matches[1] }
        elseif ($line -match "type\[\d+\].*sub=\{00000016.*size=1280x720") { $has720 = $true }
        elseif ($line -match "type\[\d+\].*size=640x480") { $has640 = $true }
    }
    if ($idx -ge 0 -and $mt -eq 3 -and $has720 -and $has640) { if ($found -lt 0) { $found = $idx } }
    if ($found -ge 0) { return $found }
    return $null
}

# $expectMoving: $true = video (кадры должны различаться), $false = static (идентичны).
function Test-Frames([string]$prefix, [bool]$expectMoving) {
    $files = Capture 5 $prefix
    if ($files.Count -lt 5) { Fail "${prefix}: captured $($files.Count) of 5 frames"; return }
    Pass "${prefix}: captured $($files.Count) frames"
    $infos = @()
    foreach ($f in $files) { $infos += Get-FrameInfo $f }
    $maxNonBlack = ($infos | Measure-Object -Property NonBlack -Maximum).Maximum
    if ($maxNonBlack -le 0) { Fail "${prefix}: all sampled pixels black (no producer frames)" }
    else { Pass "${prefix}: nonBlack (max sampled points) = $maxNonBlack" }
    $badSize = @($infos | Where-Object { $_.Width -ne 640 -or $_.Height -ne 480 })
    if ($badSize.Count -gt 0) {
        Fail "${prefix}: expected 640x480 direct-mode frames, got $($badSize[0].Width)x$($badSize[0].Height) (SetMediaType NV12 640x480 regression?)"
    } else {
        Pass "${prefix}: frames are 640x480"
    }
    $lbErr = Test-Letterbox640 $files[0]
    if ($lbErr) { Fail "${prefix}: letterbox: $lbErr" }
    else { Pass "${prefix}: letterbox ok (640x480, black bars, content centered)" }
    $unique = ($infos | Select-Object -ExpandProperty Hash -Unique).Count
    if ($expectMoving) {
        if ($unique -gt 1) { Pass "${prefix}: frames differ (moving video, unique=$unique)" }
        else { Fail "${prefix}: frames identical, moving video expected" }
    } else {
        if ($unique -eq 1) { Pass "${prefix}: frames identical (static source)" }
        else { Fail "${prefix}: frames differ (unique=$unique), static source expected" }
    }
}

function Stop-Cli($proc) {
    if ($proc -and -not $proc.HasExited) {
        Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
        $proc.WaitForExit(5000) | Out-Null
    }
}

# --- precondition: single writer AND nothing running from build outputs.
# Two writers interleave frames (every moving/static assert becomes
# unreliable), and a running exe is locked -> MSB3027 during the build below. ---
$hostProcs = @(Get-Process -Name "VCamVideoStreamProducer", "VCamProducerCli", "VCamSettingsUi", "VCamPreview" -ErrorAction SilentlyContinue)
if ($hostProcs.Count -gt 0) {
    Write-Host "FAIL: process(es) running: $(($hostProcs | ForEach-Object { "$($_.ProcessName) pid=$($_.Id)" }) -join ', ')" -ForegroundColor Red
    Write-Host "Stop them first:  Stop-Process -Name VCamVideoStreamProducer,VCamProducerCli,VCamSettingsUi,VCamPreview -Force   (then rerun e2e)" -ForegroundColor Red
    exit 1
}

Write-Host "=== 1. Building Solution ==="
& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" (Join-Path $Root "VirtualCameraMediaSource.sln") /p:Configuration=Release /p:Platform=x64 /m /v:minimal /nologo
if ($LASTEXITCODE -ne 0) {
    Write-Error "Build failed!"
    exit 1
}

Write-Host "=== 2. Registering MediaSource.dll ==="
& "C:\Windows\System32\regsvr32.exe" /s $MediaDll
$regCode = $LASTEXITCODE
if ($regCode -eq 0) {
    Pass "regsvr32: build path registered"
} else {
    Write-Host "INFO: regsvr32 exit=$regCode (non-elevated is a no-op) - direct mode is covered by the HKCU per-user registration below; device mode tests the installed copy (hash guard below)" -ForegroundColor Yellow
}

Write-Host "=== 3. Preparing Test Image ==="
$TestImageLocal = $TestImage
if (-not (Test-Path $TestImageLocal)) {
    # Deterministic fallback: generate the static test image instead of a
    # hardcoded machine path (1280x720 flat non-black color; static-source
    # asserts only need identical, non-black, letterboxed frames).
    $gen = New-Object System.Drawing.Bitmap 1280, 720
    $gfx = [System.Drawing.Graphics]::FromImage($gen)
    $gfx.Clear([System.Drawing.Color]::FromArgb(30, 90, 160))
    $gfx.Dispose()
    $gen.Save($TestImageLocal, [System.Drawing.Imaging.ImageFormat]::Bmp)
    $gen.Dispose()
    Pass "test image generated: $TestImageLocal"
}
if (-not (Test-Path $TestVideo)) {
    Write-Error "Test video not found: $TestVideo"
    exit 1
}

# --- per-user COM registration: points CoCreateInstance at the FRESHLY BUILT
# DLL (HKCU\Software\Classes\CLSID wins over HKLM). Non-elevated regsvr32 above
# cannot write HKLM, so without this the direct phases would silently test
# C:\Program Files\VCam\MediaSource.dll instead of the build. ---
$ClsidPrev = $null
$ClsidKeyCreated = $false
if (Test-Path "$ClsidKey\InprocServer32") {
    $ClsidPrev = (Get-ItemProperty -Path "$ClsidKey\InprocServer32" -Name "(default)")."(default)"
} else {
    New-Item -Path "$ClsidKey\InprocServer32" -Force | Out-Null
    $ClsidKeyCreated = $true
}
Set-ItemProperty -Path "$ClsidKey\InprocServer32" -Name "(default)" -Value $MediaDll
# ThreadingModel=Both: without it COM marshals MTA-client QI through a proxy
# that returns E_NOINTERFACE for IMFMediaSource2 (direct phases get 0 frames).
Set-ItemProperty -Path "$ClsidKey\InprocServer32" -Name "ThreadingModel" -Value "Both"
Pass "HKCU CLSID -> $MediaDll (direct mode tests the build)"

if (-not (Test-Path $OutDir)) { New-Item -ItemType Directory -Path $OutDir | Out-Null }

# --- device phases (D/E) go through the FrameServer proxy, which loads the
# INSTALLED copy of the DLL - guard that it is the fresh build. ---
$DevicePhaseOk = $true
if (-not (Test-Path $InstalledDll)) {
    Fail "device phases: $InstalledDll not found - run the deploy ritual first"
    $DevicePhaseOk = $false
} else {
    $hBuild = (Get-FileHash $MediaDll -Algorithm SHA256).Hash
    $hInst = (Get-FileHash $InstalledDll -Algorithm SHA256).Hash
    if ($hBuild -eq $hInst) { Pass "installed DLL == freshly built DLL (device phases test the build)" }
    else {
        Fail "installed DLL is STALE (differs from build) - run the deploy ritual first; phases D/E skipped"
        $DevicePhaseOk = $false
    }
}

# msrc_diag.log window marker: everything appended after this offset is ours.
$diagOffset = 0
if (Test-Path -LiteralPath $DiagLog) { $diagOffset = (Get-Item -LiteralPath $DiagLog).Length }

$settingsExisted = Test-Path $SettingsPath
if ($settingsExisted) { Copy-Item $SettingsPath $SettingsBackup -Force }
$settingsBackupHash = if ($settingsExisted) { (Get-FileHash $SettingsPath -Algorithm SHA256).Hash } else { $null }

$cliProc = $null
try {
    Write-Host "=== 4. Phase A: VCamProducerCli run --type video --path test_video.mp4 ==="
    $logA = Join-Path $OutDir "cli_phase_a.log"
    $errA = Join-Path $OutDir "cli_phase_a.err"
    $cliProc = Start-Process -FilePath $Cli -ArgumentList @("run", "--type", "video", "--path", $TestVideo) `
        -PassThru -WindowStyle Hidden -RedirectStandardOutput $logA -RedirectStandardError $errA
    Start-Sleep -Seconds 4
    if ($cliProc.HasExited) {
        Fail "phase A: CLI exited early with code $($cliProc.ExitCode)"
    } else {
        Pass "phase A: CLI is running (pid $($cliProc.Id))"
    }
    Test-Frames "e2e_frame" $true
    $stA = (& $Cli status | Out-String)
    if ($stA -match "frames are being written") { Pass "phase A: status - frames are being written (seq grows)" }
    else { Fail "phase A: status did not report writing: $($stA -replace '\r?\n', ' | ')" }
    Stop-Cli $cliProc
    $cliProc = $null
    $readyA = Select-String -Path $logA -Pattern "writer ready" -ErrorAction SilentlyContinue
    $activeA = Select-String -Path $logA -Pattern "\[cli\] active:" -ErrorAction SilentlyContinue
    if ($readyA) { Pass "phase A: log has 'writer ready'" } else { Fail "phase A: no 'writer ready' in $logA" }
    if ($activeA) { Pass "phase A: log has 'active' line" } else { Fail "phase A: no 'active' line in $logA" }

    Write-Host "=== 5. Phase B: hot-switch static -> video (settings.json, no overrides) ==="
    Write-TestSettings "static"
    $logB = Join-Path $OutDir "cli_phase_b.log"
    $errB = Join-Path $OutDir "cli_phase_b.err"
    $cliProc = Start-Process -FilePath $Cli -ArgumentList @("run") `
        -PassThru -WindowStyle Hidden -RedirectStandardOutput $logB -RedirectStandardError $errB
    Start-Sleep -Seconds 4
    if ($cliProc.HasExited) {
        Fail "phase B: CLI exited early with code $($cliProc.ExitCode)"
    } else {
        Pass "phase B: CLI is running (pid $($cliProc.Id))"
    }
    Test-Frames "e2e_hs_static" $false

    Write-TestSettings "video"
    Start-Sleep -Seconds 3
    Test-Frames "e2e_hs_video" $true
    $stB = (& $Cli status | Out-String)
    if ($stB -match "frames are being written") { Pass "phase B: status - frames are being written (seq grows)" }
    else { Fail "phase B: status did not report writing: $($stB -replace '\r?\n', ' | ')" }
    Stop-Cli $cliProc
    $cliProc = $null

    $switches = @(Select-String -Path $logB -Pattern "\[cli\] switch:" -ErrorAction SilentlyContinue)
    if ($switches.Count -ge 2) { Pass "phase B: $($switches.Count) '[cli] switch' lines in log (static + video)" }
    else { Fail "phase B: expected >=2 switch lines, got $($switches.Count)" }
    $opened = @(Select-String -Path $logB -Pattern "\[cli\] source opened: type=video" -ErrorAction SilentlyContinue)
    if ($opened.Count -ge 1) { Pass "phase B: video source opened after settings change" }
    else { Fail "phase B: video source never opened" }

    Write-Host "=== 6. Phase C: camera source (list-devices + run --type camera) ==="
    $devLog = Join-Path $OutDir "cli_list_devices.log"
    $devErr = Join-Path $OutDir "cli_list_devices.err"
    $devProc = Start-Process -FilePath $Cli -ArgumentList @("list-devices") `
        -PassThru -WindowStyle Hidden -RedirectStandardOutput $devLog -RedirectStandardError $devErr -Wait
    if ($devProc.ExitCode -ne 0) { Fail "phase C: list-devices exit code $($devProc.ExitCode)" }
    # stdout = строки "<id>\t<name>" (шапка уходит в stderr и сюда не попадает)
    $devices = @()
    foreach ($line in @(Get-Content -LiteralPath $devLog -Encoding UTF8 -ErrorAction SilentlyContinue)) {
        $tab = $line.IndexOf("`t")
        if ($tab -gt 0) {
            $devices += [pscustomobject]@{ Id = $line.Substring(0, $tab); Name = $line.Substring($tab + 1) }
        }
    }
    if ($devices.Count -eq 0) {
        Pass "SKIP: no camera device"
    } else {
        $camId = $devices[0].Id
        Write-Host "phase C: $($devices.Count) device(s), first: '$($devices[0].Name)' id=$camId"

        # --- позитивный прогон: первое реальное устройство ---
        $logC = Join-Path $OutDir "cli_phase_c.log"
        $errC = Join-Path $OutDir "cli_phase_c.err"
        $cliProc = Start-Process -FilePath $Cli -ArgumentList @("run", "--type", "camera", "--device", $camId) `
            -PassThru -WindowStyle Hidden -RedirectStandardOutput $logC -RedirectStandardError $errC
        $readyC = $null; $openedC = $null; $activeC = $null
        $deadline = (Get-Date).AddSeconds(12)
        do {
            if ($cliProc.HasExited) { break }
            $readyC = Select-String -Path $logC -Pattern "writer ready" -ErrorAction SilentlyContinue
            $openedC = Select-String -Path $logC -Pattern "\[cli\] source opened: type=camera" -ErrorAction SilentlyContinue
            $activeC = Select-String -Path $logC -Pattern "\[cli\] active:" -ErrorAction SilentlyContinue
            if ($readyC -and $openedC -and $activeC) { break }
            Start-Sleep -Milliseconds 500
        } while ((Get-Date) -lt $deadline)

        if ($cliProc.HasExited) { Fail "phase C: CLI exited early with code $($cliProc.ExitCode)" }
        if ($readyC) { Pass "phase C: log has 'writer ready'" } else { Fail "phase C: no 'writer ready' in $logC" }

        if (-not ($openedC -and $activeC)) {
            # Камера недоступна (занята другим процессом / отключена): НЕ подгоняем
            # проверку - фаза C уходит в SKIP, e2e остаётся exit 0.
            Write-Host "PHASE C NOTE: camera open/active not reached - device may be busy:" -ForegroundColor Yellow
            Select-String -Path $logC -Pattern "\[cli\] (open failed|no signal)" -ErrorAction SilentlyContinue |
                Select-Object -First 5 | ForEach-Object { Write-Host "  $($_.Line)" -ForegroundColor Yellow }
            Pass "SKIP: phase C camera unavailable (opened=$([bool]$openedC) active=$([bool]$activeC)) - see $logC"
        } else {
            Pass "phase C: 'source opened: type=camera' + 'active' in log"
            $stOut = (& $Cli status | Out-String)
            if ($stOut -match "frames are being written") { Pass "phase C: seq grows over 300 ms (status)" }
            else { Fail "phase C: seq did not grow over 300 ms: $($stOut -replace '\r?\n', ' | ')" }
            # Пиксельные проверки - только informational: тёмный/статичный кадр
            # (камера смотрит в тёмную комнату) - не баг.
            $camFrames = Capture 3 "e2e_cam"
            if ($camFrames.Count -ge 1) {
                $infos = @()
                foreach ($f in $camFrames) { $infos += Get-FrameInfo $f }
                $nbMax = ($infos | Measure-Object -Property NonBlack -Maximum).Maximum
                $uniq = ($infos | Select-Object -ExpandProperty Hash -Unique).Count
                Write-Host "INFO: phase C pixels (informational only, dark scene is OK): nonBlack(max)=$nbMax unique=$uniq" -ForegroundColor Yellow
            } else {
                Write-Host "INFO: phase C: no frames captured for pixel inspection" -ForegroundColor Yellow
            }
        }
        Stop-Cli $cliProc
        $cliProc = $null

        # --- негативная проверка: несуществующий id -> NO SIGNAL, никогда active ---
        $logD = Join-Path $OutDir "cli_phase_c_neg.log"
        $errD = Join-Path $OutDir "cli_phase_c_neg.err"
        $cliProc = Start-Process -FilePath $Cli -ArgumentList @("run", "--type", "camera", "--device", "\\?\nonexistent") `
            -PassThru -WindowStyle Hidden -RedirectStandardOutput $logD -RedirectStandardError $errD
        Start-Sleep -Seconds 7
        $negExited = $cliProc.HasExited
        $negCode = if ($negExited) { $cliProc.ExitCode } else { $null }
        $noSigD = Select-String -Path $logD -Pattern "\[cli\] no signal" -ErrorAction SilentlyContinue
        $activeD = Select-String -Path $logD -Pattern "\[cli\] active:" -ErrorAction SilentlyContinue
        $retryD = @(Select-String -Path $logD -Pattern "\[cli\] open failed" -ErrorAction SilentlyContinue)
        Stop-Cli $cliProc
        $cliProc = $null
        if ($negExited) {
            Fail "phase C negative: CLI exited early with code $negCode (expected to keep retrying)"
        } elseif ($activeD) {
            Fail "phase C negative: became active with a nonexistent device id"
        } elseif (-not $noSigD) {
            Fail "phase C negative: no '[cli] no signal' line in $logD within 7 s"
        } elseif ($retryD.Count -lt 1) {
            Fail "phase C negative: no signal but no 'open failed' retries in $logD"
        } else {
            Pass "phase C negative: no signal fallback within 7 s, never active (open retries=$($retryD.Count))"
        }
    }

    # ===== Phase D/E: device mode - the FrameServer proxy path (what ktalk uses) =====
    Write-Host "=== 7. Phase D/E: device mode (proxy): 1280x720 + 640x480 RGB32 ==="
    $deBusy = $false
    if (-not $DevicePhaseOk) {
        Pass "SKIP: phases D/E (installed DLL is not the build - deploy first)"
    } else {
        $vcamIdx = Find-VCamDevice
        if ($null -eq $vcamIdx) {
            Fail "phases D/E: VCam device not enumerated (FrameServer running? 'Registrar.exe add VCam' done?)"
        } else {
            Pass "phases D/E: VCam device at index $vcamIdx (mediaTypes=3)"
            $logDE = Join-Path $OutDir "cli_phase_de.log"
            $errDE = Join-Path $OutDir "cli_phase_de.err"
            $cliProc = Start-Process -FilePath $Cli -ArgumentList @("run") `
                -PassThru -WindowStyle Hidden -RedirectStandardOutput $logDE -RedirectStandardError $errDE
            Start-Sleep -Seconds 4
            if ($cliProc.HasExited) {
                Fail "phase D: CLI exited early with code $($cliProc.ExitCode)"
                Stop-Cli $cliProc
                $cliProc = $null
            } else {
                Pass "phase D: CLI is running (pid $($cliProc.Id))"

                # --- D: 1280x720 RGB32 through the proxy (shared allocator path) ---
                $resD = Capture-Device $vcamIdx 1280 720 "e2e_dev720"
                if ($resD.ExitCode -ne 0) {
                    $devText = Get-Content -LiteralPath $resD.LogPath -Raw -ErrorAction SilentlyContinue
                    if ($devText -match "0xC00D3E9B|WRONGSTATE") {
                        $deBusy = $true
                        Pass "SKIP: phases D/E - VCam session busy (another consumer holds the camera) - see $($resD.LogPath)"
                    } else {
                        Fail "phase D: CaptureTest device 1280x720 exit $($resD.ExitCode) - see $($resD.LogPath)"
                    }
                } elseif ($resD.Files.Count -lt 5) {
                    Fail "phase D: captured $($resD.Files.Count) of 10 frames"
                } else {
                    Pass "phase D: captured $($resD.Files.Count) frames (1280x720 RGB32 via proxy)"
                    $dErr = Test-FullHeight720 $resD.Files[0]
                    if ($dErr) { Fail "phase D: $dErr" }
                    else { Pass "phase D: 1280x720, content reaches top and bottom (no top-only truncation)" }
                    $winD = Read-DiagWindow $diagOffset
                    if ($winD -match "buffer too small") { Fail "phase D: msrc_diag.log contains 'buffer too small'" }
                    else { Pass "phase D: no 'buffer too small' in msrc_diag.log window" }
                    if ($winD -match "negotiated 1280x720 RGB32 -> allocator type RGB720") { Pass "phase D: StartForSession negotiated 1280x720 RGB32 -> RGB720" }
                    else { Fail "phase D: no 'negotiated 1280x720 RGB32 -> allocator type RGB720' line in diag window" }
                    if ($winD -match "Lock hr=0x00000000[^\r\n]*maxLen=3686400") { Pass "phase D: shared allocator Lock maxLen=3686400" }
                    else { Fail "phase D: no shared-allocator Lock with maxLen=3686400 in diag window" }
                }

                # --- E: 640x480 RGB32 - the exact type ktalk negotiates ---
                if (-not $deBusy -and $resD.ExitCode -eq 0 -and $resD.Files.Count -ge 5) {
                    $resE = Capture-Device $vcamIdx 640 480 "e2e_dev640"
                    if ($resE.ExitCode -ne 0) {
                        Fail "phase E: CaptureTest device 640x480 exit $($resE.ExitCode) - see $($resE.LogPath)"
                    } elseif ($resE.Files.Count -lt 5) {
                        Fail "phase E: captured $($resE.Files.Count) of 10 frames"
                    } else {
                        Pass "phase E: captured $($resE.Files.Count) frames (640x480 RGB32 via proxy = ktalk path)"
                        $eErr = Test-Letterbox640 $resE.Files[0]
                        if ($eErr) { Fail "phase E: $eErr" }
                        else { Pass "phase E: letterbox ok (aspect preserved, bars black)" }
                        $eRows = Test-RowsNonBlack $resE.Files[0] @(75, 240, 410)
                        if ($eRows) { Fail "phase E: content: $eRows" }
                        else { Pass "phase E: content rows non-black (frame not truncated)" }
                        $winE = Read-DiagWindow $diagOffset
                        if ($winE -match "buffer too small") { Fail "phase E: msrc_diag.log contains 'buffer too small'" }
                        else { Pass "phase E: no 'buffer too small' in msrc_diag.log window" }
                        if ($winE -match "negotiated 640x480 RGB32 -> allocator type RGB640") { Pass "phase E: StartForSession negotiated 640x480 RGB32 -> RGB640" }
                        else { Fail "phase E: no 'negotiated 640x480 RGB32 -> allocator type RGB640' line in diag window" }
                        if ($winE -match "Lock hr=0x00000000[^\r\n]*maxLen=1228800") { Pass "phase E: shared allocator Lock maxLen=1228800" }
                        else { Fail "phase E: no shared-allocator Lock with maxLen=1228800 in diag window" }
                    }
                }
                Stop-Cli $cliProc
                $cliProc = $null
            }
        }
    }

    # ===== Phase F: producer stop -> frozen fallback -> restart -> moving =====
    Write-Host "=== 8. Phase F: producer stop -> frozen fallback -> restart -> moving ==="
    $logF = Join-Path $OutDir "cli_phase_f.log"
    $errF = Join-Path $OutDir "cli_phase_f.err"
    $cliProc = Start-Process -FilePath $Cli -ArgumentList @("run") `
        -PassThru -WindowStyle Hidden -RedirectStandardOutput $logF -RedirectStandardError $errF
    Start-Sleep -Seconds 4
    if ($cliProc.HasExited) {
        Fail "phase F: CLI exited early with code $($cliProc.ExitCode)"
        Stop-Cli $cliProc
        $cliProc = $null
    } else {
        Stop-Cli $cliProc
        $cliProc = $null
        Start-Sleep -Seconds 1
        $frozen = Capture 3 "e2e_frozen"
        if ($frozen.Count -lt 3) {
            Fail "phase F: captured $($frozen.Count) of 3 frozen frames"
        } else {
            $infosFrozen = @()
            foreach ($f in $frozen) { $infosFrozen += Get-FrameInfo $f }
            $uniqFrozen = ($infosFrozen | Select-Object -ExpandProperty Hash -Unique).Count
            $nbFrozen = ($infosFrozen | Measure-Object -Property NonBlack -Maximum).Maximum
            if ($uniqFrozen -eq 1) { Pass "phase F: frames identical after writer stop (fallback: last-frame cache / NO SIGNAL)" }
            else { Fail "phase F: frames differ after writer stop (unique=$uniqFrozen)" }
            if ($nbFrozen -gt 0) { Pass "phase F: frozen frames non-black" }
            else { Fail "phase F: frozen frames are all black" }
        }
        $logF2 = Join-Path $OutDir "cli_phase_f2.log"
        $errF2 = Join-Path $OutDir "cli_phase_f2.err"
        $cliProc = Start-Process -FilePath $Cli -ArgumentList @("run") `
            -PassThru -WindowStyle Hidden -RedirectStandardOutput $logF2 -RedirectStandardError $errF2
        Start-Sleep -Seconds 4
        if ($cliProc.HasExited) {
            Fail "phase F: CLI restart exited early with code $($cliProc.ExitCode)"
            Stop-Cli $cliProc
            $cliProc = $null
        } else {
            $rec = Capture 3 "e2e_recovered"
            if ($rec.Count -lt 3) {
                Fail "phase F: captured $($rec.Count) of 3 recovery frames"
            } else {
                $infosRec = @()
                foreach ($f in $rec) { $infosRec += Get-FrameInfo $f }
                $uniqRec = ($infosRec | Select-Object -ExpandProperty Hash -Unique).Count
                if ($uniqRec -gt 1) { Pass "phase F: frames moving again after restart (unique=$uniqRec)" }
                else { Fail "phase F: frames identical after restart (producer did not recover?)" }
            }
            Stop-Cli $cliProc
            $cliProc = $null
        }
    }
}
finally {
    Stop-Cli $cliProc
    # restore the per-user COM registration state
    if ($null -ne $ClsidPrev) {
        Set-ItemProperty -Path "$ClsidKey\InprocServer32" -Name "(default)" -Value $ClsidPrev
        Pass "HKCU CLSID restored to previous value"
    } elseif ($ClsidKeyCreated) {
        Remove-Item -Recurse -Force $ClsidKey -ErrorAction SilentlyContinue
        if (-not (Test-Path $ClsidKey)) { Pass "HKCU CLSID test registration removed" }
        else { Fail "could not remove HKCU CLSID key $ClsidKey" }
    }
    if ($settingsExisted) {
        Copy-Item $SettingsBackup $SettingsPath -Force
        $nowHash = (Get-FileHash $SettingsPath -Algorithm SHA256).Hash
        if ($nowHash -eq $settingsBackupHash) { Pass "settings.json restored byte-for-byte (SHA256 match)" }
        else { Fail "settings.json restore mismatch!" }
        Remove-Item $SettingsBackup -Force -ErrorAction SilentlyContinue
    } else {
        if (Test-Path $SettingsPath) {
            Remove-Item $SettingsPath -Force
            Pass "settings.json did not exist before test - removed the test file"
        }
    }
}

Write-Host "=== 9. Result ==="
if ($script:Failures.Count -eq 0) {
    Write-Host "SUCCESS: E2E test passed ($($script:Failures.Count) failures)" -ForegroundColor Green
    exit 0
} else {
    Write-Host "FAILURE: $($script:Failures.Count) check(s) failed:" -ForegroundColor Red
    $script:Failures | ForEach-Object { Write-Host "  - $_" -ForegroundColor Red }
    exit 1
}
