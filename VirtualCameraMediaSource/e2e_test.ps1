# E2E test: frame producer (VCamProducerCli) -> shared memory -> camera -> CaptureTest.
# Phases:
#   A) run --type video --path test_video.mp4 (CLI overrides) -> moving frames
#      + status "frames are being written", BMP must be 640x480 with letterbox
#   B) run (no overrides) + settings.json writes -> hot-switch static -> video
#   G) dual-write v1+v2 (vcam-quality-v2): v1 header ver=1 1280x720 slots=8 +
#      v2 header ver=2 slots=4, seq grows in BOTH sections, status shows
#      quality + v2; v1 pixels keep moving/letterboxed (video source).
#      v1 bit-notes: 720p-matched static input is a memcpy (bit-exact); video
#      input is native-decode + our downscale (Sub1 risk: within tolerance).
#   H) ladder with a >720p source (vcam-quality-v2): generated 1080p static
#      (ffmpeg testsrc2, fallback System.Drawing) -> CLI log "native 1920x1080",
#      v2 dims == 1920x1080, static frames identical, `inspect` lists 4 media
#      types incl. size=1920x1080; then quality fixed720p hot-switch (no
#      restart) -> v2 == 720p -> back to source -> v2 == 1920x1080.
#      Temp file e2e_1080p.bmp lives in OUTDIR and is removed in `finally`.
#      (Physical Brio on this machine is 640x480 <720p — ladder-up is covered
#      by the generated static, not by phase C.)
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
$TestImage1080 = Join-Path $OutDir "e2e_1080p.bmp" # generated 1080p static (phase H), removed in finally
$SettingsBackup = Join-Path $env:TEMP "vcam_e2e_settings_backup.json"
$Utf8NoBom = New-Object System.Text.UTF8Encoding($false)
$VCamClsid = "{B2B674D4-9CF0-461C-BDCE-3D56FBB41356}"
$ClsidKey = "HKCU:\Software\Classes\CLSID\$VCamClsid"
$InstalledDll = Join-Path $env:ProgramFiles "VCam\MediaSource.dll"
# MediaSource writes its diag log to %ProgramData%\VCam\msrc_diag.log (shared
# for every context; the installer/deploy ritual grants LOCAL SERVICE+Users
# modify on that dir) plus <GetTempPath>\VCam\msrc_diag.log of the PROCESS that
# loaded it (user context -> %TEMP%, svchost -> its own temp). Read the window
# from both so device-mode lines are seen regardless of context.
$DiagLogFiles = @(
    (Join-Path $env:ProgramData "VCam\msrc_diag.log"),
    (Join-Path $env:TEMP "VCam\msrc_diag.log")
)

Add-Type -AssemblyName System.Drawing

$script:Failures = New-Object System.Collections.Generic.List[string]
function Fail([string]$msg) { $script:Failures.Add($msg); Write-Host "FAIL: $msg" -ForegroundColor Red }
function Pass([string]$msg) { Write-Host "PASS: $msg" -ForegroundColor Green }

# MSBuild location: vswhere (any VS edition/version), fallback to PATH.
function Find-MSBuild {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path -LiteralPath $vswhere) {
        $found = & $vswhere -latest -requires Microsoft.Component.MSBuild -find "MSBuild\**\Bin\MSBuild.exe" 2>$null | Select-Object -First 1
        if ($found -and (Test-Path -LiteralPath $found)) { return $found }
    }
    $cmd = Get-Command msbuild.exe -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    Fail "MSBuild not found: vswhere.exe has no result and msbuild.exe is not on PATH"
    exit 1
}

function Write-TestSettings([string]$type, [string]$quality = "source", [string]$staticPath = "") {
    if ($staticPath -eq "") { $staticPath = $TestImage }
    $img = $staticPath -replace '\\', '\\'
    $vid = $TestVideo -replace '\\', '\\'
    $json = @"
{
  "source": { "type": "$type" },
  "static": { "path": "$img", "scaleMode": "fit", "cropX": 0, "cropY": 0, "cropW": 0, "cropH": 0, "cropKeepAspect": false },
  "video": { "path": "$vid" },
  "quality": "$quality",
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

# Reads only the part of every diag log appended after its offset in $offsets.
function Read-DiagWindow([hashtable]$offsets) {
    $parts = New-Object System.Collections.Generic.List[string]
    foreach ($p in $DiagLogFiles) {
        if (-not (Test-Path -LiteralPath $p)) { continue }
        $off = 0L
        if ($offsets -and $offsets.ContainsKey($p)) { $off = [long]$offsets[$p] }
        $fs = [IO.File]::Open($p, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite)
        try {
            if ($off -gt $fs.Length) { $off = 0 }
            $fs.Seek($off, [IO.SeekOrigin]::Begin) | Out-Null
            $len = $fs.Length - $off
            if ($len -le 0) { continue }
            $buf = New-Object byte[] $len
            $read = 0
            while ($read -lt $len) {
                $n = $fs.Read($buf, $read, $len - $read)
                if ($n -le 0) { break }
                $read += $n
            }
            $parts.Add([Text.Encoding]::UTF8.GetString($buf, 0, $read))
        } finally { $fs.Dispose() }
    }
    return ($parts -join "`n")
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
# at least 3 media types (RGB32 1280x720 + NV12 1280x720 + RGB32 640x480); the v2 quality ladder may add more (native size).
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
            if ($idx -ge 0 -and $mt -ge 3 -and $has720 -and $has640) { if ($found -lt 0) { $found = $idx } }
            $idx = [int]$Matches[1]; $mt = -1; $has720 = $false; $has640 = $false
        } elseif ($line -match "mediaTypes=(\d+)") { $mt = [int]$Matches[1] }
        elseif ($line -match "type\[\d+\].*sub=\{00000016.*size=1280x720") { $has720 = $true }
        elseif ($line -match "type\[\d+\].*size=640x480") { $has640 = $true }
    }
    if ($idx -ge 0 -and $mt -ge 3 -and $has720 -and $has640) { if ($found -lt 0) { $found = $idx } }
    if ($found -ge 0) { return $found }
    return $null
}

# Reads a v1/v2 section header read-only (Global -> Local, like the readers):
# 9xUINT32 (magic..frameWriteIndex) + seq int64 at offset 36. Returns $null
# when neither prefix exists. seq is sampled twice (300 ms) for growth.
function Read-SectionHeader([string]$base) {
    foreach ($pre in @("Global\", "Local\")) {
        $name = $pre + $base
        try {
            $mmf = [IO.MemoryMappedFiles.MemoryMappedFile]::OpenExisting(
                $name, [IO.MemoryMappedFiles.MemoryMappedFileRights]::Read)
        } catch { continue }
        try {
            $acc = $mmf.CreateViewAccessor(0, 72, [IO.MemoryMappedFiles.MemoryMappedFileAccess]::Read)
            try {
                $seq1 = 0
                try {
                    $magic = $acc.ReadUInt32(0); $ver = $acc.ReadUInt32(4)
                    $w = $acc.ReadUInt32(8); $h = $acc.ReadUInt32(12)
                    $stride = $acc.ReadUInt32(16); $pix = $acc.ReadUInt32(20)
                    $fsize = $acc.ReadUInt32(24); $slots = $acc.ReadUInt32(28)
                    $seq1 = $acc.ReadInt64(36)
                    Start-Sleep -Milliseconds 300
                    $seq2 = $acc.ReadInt64(36)
                } catch { continue }
                return [pscustomobject]@{ Name = $name; Magic = $magic; Version = $ver;
                    W = $w; H = $h; Stride = $stride; PixelFormat = $pix;
                    FrameSize = $fsize; Slots = $slots;
                    Seq1 = $seq1; Seq2 = $seq2; Grows = ($seq2 -ne $seq1) }
            } finally { $acc.Dispose() }
        } finally { $mmf.Dispose() }
    }
    return $null
}

# Phase G asserts (vcam-quality-v2 dual-write): v1 header frozen by contract
# (ver=1 1280x720 stride=5120 slots=8) + v2 header (ver=2 slots=4, sane dims),
# seq grows in BOTH sections while the producer runs.
function Test-DualWriteHeaders {
    $v1 = Read-SectionHeader "VCam.FrameBuffer.v1"
    if ($null -eq $v1) { Fail "phase G: v1 section not open" }
    else {
        if ($v1.Magic -ne 0x5643414D) { Fail ("phase G: v1 BAD magic 0x{0:X8}" -f $v1.Magic) }
        elseif ($v1.Version -ne 1 -or $v1.W -ne 1280 -or $v1.H -ne 720 -or $v1.Stride -ne 5120 -or $v1.Slots -ne 8) {
            Fail "phase G: v1 header ver=$($v1.Version) $($v1.W)x$($v1.H) stride=$($v1.Stride) slots=$($v1.Slots) (want ver=1 1280x720 stride=5120 slots=8)"
        } else { Pass "phase G: v1 header ver=1 1280x720 stride=5120 slots=8 ($($v1.Name))" }
        if ($v1.Grows) { Pass "phase G: v1 seq grows ($($v1.Seq1) -> $($v1.Seq2))" }
        else { Fail "phase G: v1 seq frozen ($($v1.Seq1))" }
    }
    $v2 = Read-SectionHeader "VCam.FrameBuffer.v2"
    if ($null -eq $v2) { Fail "phase G: v2 section not open (dual-write regression?)" }
    else {
        if ($v2.Magic -ne 0x5643414D) { Fail ("phase G: v2 BAD magic 0x{0:X8}" -f $v2.Magic) }
        elseif ($v2.Version -ne 2 -or $v2.Slots -ne 4) {
            Fail "phase G: v2 header ver=$($v2.Version) slots=$($v2.Slots) (want ver=2 slots=4)"
        } else { Pass "phase G: v2 header ver=2 slots=4 $($v2.W)x$($v2.H) stride=$($v2.Stride) ($($v2.Name))" }
        if ($v2.W -le 0 -or $v2.H -le 0 -or $v2.W -gt 3840 -or $v2.H -gt 2160) {
            Fail "phase G: v2 dims out of range ($($v2.W)x$($v2.H), cap 3840x2160)"
        }
        if ($v2.Grows) { Pass "phase G: v2 seq grows ($($v2.Seq1) -> $($v2.Seq2))" }
        else { Fail "phase G: v2 seq frozen ($($v2.Seq1))" }
    }
}

# Finds the VCam device index whose ladder includes a native type WxH
# (vcam-quality-v2 phase H: mediaTypes=4 with size=1920x1080). Returns $null
# when the native type is not advertised.
function Find-VCamDeviceNative([int]$w, [int]$h) {
    $logPath = Join-Path $OutDir "inspect_vcam_native.log"
    $errPath = Join-Path $OutDir "inspect_vcam_native.err"
    $p = Start-Process -FilePath $CaptureTest -ArgumentList @("inspect") `
        -PassThru -WindowStyle Hidden -RedirectStandardOutput $logPath -RedirectStandardError $errPath -Wait
    if ($p.ExitCode -ne 0) { return $null }
    $idx = -1; $found = -1
    foreach ($line in @(Get-Content -LiteralPath $logPath)) {
        if ($line -match "device\[(\d+)\]") { $idx = [int]$Matches[1] }
        elseif ($line -match "size=${w}x${h}") { if ($idx -ge 0 -and $found -lt 0) { $found = $idx } }
    }
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
& (Find-MSBuild) (Join-Path $Root "VirtualCameraMediaSource.sln") /p:Configuration=Release /p:Platform=x64 /m /v:minimal /nologo
if ($LASTEXITCODE -ne 0) {
    Write-Error "Build failed!"
    exit 1
}

Write-Host "=== 2. Registering MediaSource.dll ==="
& (Join-Path $env:WINDIR "System32\regsvr32.exe") /s $MediaDll
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
# $InstalledDll instead of the build. ---
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

# Diag log window markers: everything appended after these offsets is ours.
$diagOffsets = @{}
foreach ($dlf in $DiagLogFiles) {
    if (Test-Path -LiteralPath $dlf) { $diagOffsets[$dlf] = (Get-Item -LiteralPath $dlf).Length }
}

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

    Write-Host "=== 5b. Phase G: dual-write v1+v2 (video, moving) ==="
    # settings.json остался video/source из фазы B — отдельный CLI run без overrides.
    $logG = Join-Path $OutDir "cli_phase_g2.log"
    $errG = Join-Path $OutDir "cli_phase_g2.err"
    $cliProc = Start-Process -FilePath $Cli -ArgumentList @("run") `
        -PassThru -WindowStyle Hidden -RedirectStandardOutput $logG -RedirectStandardError $errG
    Start-Sleep -Seconds 4
    if ($cliProc.HasExited) {
        Fail "phase G: CLI exited early with code $($cliProc.ExitCode)"
    } else {
        Pass "phase G: CLI is running (pid $($cliProc.Id))"
    }
    $stG = (& $Cli status | Out-String)
    if ($stG -match "frames are being written") { Pass "phase G: status v1 - frames are being written (seq grows)" }
    else { Fail "phase G: status v1 did not report writing: $($stG -replace '\r?\n', ' | ')" }
    if ($stG -match "quality:\s*source") { Pass "phase G: status shows 'quality: source'" }
    else { Fail "phase G: status has no 'quality: source': $($stG -replace '\r?\n', ' | ')" }
    if ($stG -match "v2 section .* frames are being written") { Pass "phase G: status v2 - frames are being written (seq grows)" }
    else { Fail "phase G: status v2 did not report writing: $($stG -replace '\r?\n', ' | ')" }
    Test-DualWriteHeaders
    Test-Frames "e2e_dual" $true
    Stop-Cli $cliProc
    $cliProc = $null

    Write-Host "=== 5c. Phase H: ladder - native 1080p source advertises 4 types ==="
    # Источник >720p для лесенки: генерируем 1080p-BMP в OUTDIR (ffmpeg testsrc2;
    # fallback — System.Drawing заливка). Brio-камера машины 640x480 <720p и для
    # лесенки вверх не годится — покрытие идёт generated-static (см. шапку).
    $gen1080 = Test-Path -LiteralPath $TestImage1080
    if (-not $gen1080) {
        $ff = Get-Command ffmpeg -ErrorAction SilentlyContinue
        if ($ff) {
            & $ff.Source -y -loglevel error -f lavfi -i "testsrc2=size=1920x1080:rate=30:duration=1" -frames:v 1 $TestImage1080
            $gen1080 = Test-Path -LiteralPath $TestImage1080
            if ($gen1080) { Pass "phase H: 1080p test image generated via ffmpeg" }
        }
    }
    if (-not $gen1080) {
        try {
            $gen = New-Object System.Drawing.Bitmap 1920, 1080
            $gfx = [System.Drawing.Graphics]::FromImage($gen)
            $gfx.Clear([System.Drawing.Color]::FromArgb(40, 120, 200))
            $gfx.Dispose()
            $gen.Save($TestImage1080, [System.Drawing.Imaging.ImageFormat]::Bmp)
            $gen.Dispose()
            $gen1080 = $true
            Pass "phase H: 1080p test image generated via System.Drawing (ffmpeg missing/failed)"
        } catch {
            Fail "phase H: cannot generate 1080p test image ($_) - ladder uncovered"
        }
    }
    if ($gen1080) {
        Write-TestSettings "static" "source" $TestImage1080
        $logH = Join-Path $OutDir "cli_phase_h.log"
        $errH = Join-Path $OutDir "cli_phase_h.err"
        $cliProc = Start-Process -FilePath $Cli -ArgumentList @("run") `
            -PassThru -WindowStyle Hidden -RedirectStandardOutput $logH -RedirectStandardError $errH
        Start-Sleep -Seconds 4
        if ($cliProc.HasExited) {
            Fail "phase H: CLI exited early with code $($cliProc.ExitCode)"
        } else {
            Pass "phase H: CLI is running (pid $($cliProc.Id))"
        }
        $nativeH = Select-String -Path $logH -Pattern "\[cli\] active:.*native 1920x1080" -ErrorAction SilentlyContinue
        if ($nativeH) { Pass "phase H: host cycle renders native 1920x1080 (WriteFrameNative)" }
        else { Fail "phase H: no '[cli] active: ... native 1920x1080' in $logH" }
        $v2h = Read-SectionHeader "VCam.FrameBuffer.v2"
        if ($null -eq $v2h) { Fail "phase H: v2 section not open" }
        elseif ($v2h.W -ne 1920 -or $v2h.H -ne 1080) {
            Fail "phase H: v2 dims $($v2h.W)x$($v2h.H) (want 1920x1080 = native of the 1080p static)"
        } else { Pass "phase H: v2 dims 1920x1080 (native of the 1080p static)" }
        Test-Frames "e2e_ladder" $false
        $vcamNat = Find-VCamDeviceNative 1920 1080
        if ($null -eq $vcamNat) {
            Fail "phases H: ladder has no native 1920x1080 type (see inspect_vcam_native.log)"
        } else {
            Pass "phase H: ladder advertises native 1920x1080 (device index $vcamNat)"
        }
        # quality hot-switch без рестарта: fixed720p -> v2 == 720p -> source -> v2 == 1920x1080.
        Write-TestSettings "static" "fixed720p" $TestImage1080
        Start-Sleep -Seconds 3
        $v2f = Read-SectionHeader "VCam.FrameBuffer.v2"
        if ($null -eq $v2f) { Fail "phase H quality: v2 section not open after switch to fixed720p" }
        elseif ($v2f.W -ne 1280 -or $v2f.H -ne 720) {
            Fail "phase H quality: v2 dims $($v2f.W)x$($v2f.H) after fixed720p (want 1280x720)"
        } else { Pass "phase H quality: fixed720p hot-switch -> v2 1280x720 (no restart)" }
        $qswitch = @(Select-String -Path $logH -Pattern "\[cli\] switch:.*quality=fixed720p" -ErrorAction SilentlyContinue)
        if ($qswitch.Count -ge 1) { Pass "phase H quality: '[cli] switch: ... quality=fixed720p' in log (reopen path)" }
        else { Fail "phase H quality: no quality=fixed720p switch line in $logH" }
        Write-TestSettings "static" "source" $TestImage1080
        Start-Sleep -Seconds 3
        $v2s = Read-SectionHeader "VCam.FrameBuffer.v2"
        if ($null -eq $v2s) { Fail "phase H quality: v2 section not open after switch back to source" }
        elseif ($v2s.W -ne 1920 -or $v2s.H -ne 1080) {
            Fail "phase H quality: v2 dims $($v2s.W)x$($v2s.H) after back to source (want 1920x1080)"
        } else { Pass "phase H quality: back to source -> v2 1920x1080" }
        # fixed1080p hot-switch без рестарта: 1080p-источник идёт passthrough
        # (лесенка только вниз), в логе виден reopen с quality=fixed1080p.
        Write-TestSettings "static" "fixed1080p" $TestImage1080
        Start-Sleep -Seconds 3
        $v2t = Read-SectionHeader "VCam.FrameBuffer.v2"
        if ($null -eq $v2t) { Fail "phase H quality: v2 section not open after switch to fixed1080p" }
        elseif ($v2t.W -ne 1920 -or $v2t.H -ne 1080) {
            Fail "phase H quality: v2 dims $($v2t.W)x$($v2t.H) after fixed1080p (want 1920x1080 passthrough)"
        } else { Pass "phase H quality: fixed1080p hot-switch -> v2 1920x1080 passthrough (no restart)" }
        $qswitch = @(Select-String -Path $logH -Pattern "\[cli\] switch:.*quality=fixed1080p" -ErrorAction SilentlyContinue)
        if ($qswitch.Count -ge 1) { Pass "phase H quality: '[cli] switch: ... quality=fixed1080p' in log (reopen path)" }
        else { Fail "phase H quality: no quality=fixed1080p switch line in $logH" }
        Write-TestSettings "static" "source" $TestImage1080
        Start-Sleep -Seconds 3
        Stop-Cli $cliProc
        $cliProc = $null
        # Вернуть состояние конца фазы B (video/source): C использует overrides,
        # D/E/F — run без overrides и ждут там то же, что раньше.
        Write-TestSettings "video"
    }

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

    # Static 16:9 source for D/E: letterbox-ассерт требует не-4:3 входа.
    # (Phase H оставила video 320x240 = 4:3 -> совпадение аспектов -> нет полос.)
    Write-TestSettings "static"

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
                    $winD = Read-DiagWindow $diagOffsets
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
                        $winE = Read-DiagWindow $diagOffsets
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

    # Video source for F: test expects moving frames after restart.
    # (D/E left static 16:9 - identical frames would fail the unique>1 check.)
    Write-TestSettings "video"

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
    # Phase H temp: 1080p-BMP regenerable — не захламляем OUTDIR.
    Remove-Item -LiteralPath $TestImage1080 -Force -ErrorAction SilentlyContinue
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
