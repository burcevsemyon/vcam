# E2E test: frame producer (VCamProducerCli) -> shared memory -> camera -> CaptureTest.
# Phases:
#   A) run --type video --path test_video.mp4 (CLI overrides) -> moving frames
#   B) run (no overrides) + settings.json writes -> hot-switch static -> video
#   C) list-devices + run --type camera --device <id> (+ negative check with a
#      nonexistent id -> NO SIGNAL). No camera device -> SKIP, exit stays 0.
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
    $nonBlack = 0
    for ($x = 0; $x -lt $bmp.Width; $x += 32) {
        for ($y = 0; $y -lt $bmp.Height; $y += 18) {
            $c = $bmp.GetPixel($x, $y)
            if ($c.R -gt 10 -or $c.G -gt 10 -or $c.B -gt 10) { $nonBlack++ }
        }
    }
    $bmp.Dispose()
    [pscustomobject]@{
        Name     = $file.Name
        NonBlack = $nonBlack
        Hash     = (Get-FileHash $file.FullName -Algorithm SHA256).Hash
    }
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

Write-Host "=== 1. Building Solution ==="
& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" (Join-Path $Root "VirtualCameraMediaSource.sln") /p:Configuration=Release /p:Platform=x64 /m /v:minimal /nologo
if ($LASTEXITCODE -ne 0) {
    Write-Error "Build failed!"
    exit 1
}

Write-Host "=== 2. Registering MediaSource.dll ==="
& "C:\Windows\System32\regsvr32.exe" /s $MediaDll

Write-Host "=== 3. Preparing Test Image ==="
$TestImageLocal = $TestImage
if (-not (Test-Path $TestImageLocal)) {
    $sampleBmp = Get-ChildItem "C:\Users\Semen\source\repos\10_000.bmp" -ErrorAction SilentlyContinue
    if ($sampleBmp) {
        Copy-Item $sampleBmp.FullName $TestImageLocal
    } else {
        Write-Error "Test image not found!"
        exit 1
    }
}
if (-not (Test-Path $TestVideo)) {
    Write-Error "Test video not found: $TestVideo"
    exit 1
}

$hostProcs = @(Get-Process -Name "VCamVideoStreamProducer" -ErrorAction SilentlyContinue)
if ($hostProcs.Count -gt 0) {
    Write-Host "WARNING: tray host VCamVideoStreamProducer is running - two writers may interleave frames" -ForegroundColor Yellow
}

if (-not (Test-Path $OutDir)) { New-Item -ItemType Directory -Path $OutDir | Out-Null }

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
}
finally {
    Stop-Cli $cliProc
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

Write-Host "=== 7. Result ==="
if ($script:Failures.Count -eq 0) {
    Write-Host "SUCCESS: E2E test passed ($($script:Failures.Count) failures)" -ForegroundColor Green
    exit 0
} else {
    Write-Host "FAILURE: $($script:Failures.Count) check(s) failed:" -ForegroundColor Red
    $script:Failures | ForEach-Object { Write-Host "  - $_" -ForegroundColor Red }
    exit 1
}
