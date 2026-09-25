# E2E Test for VCam with StaticProducer (Direct Mode with Producer running)
$ErrorActionPreference = "Stop"

$Root = $PSScriptRoot
$BuildDir = Join-Path $Root "build\x64\Release"
$MediaDll = Join-Path $BuildDir "MediaSource.dll"
$StaticProducer = Join-Path $BuildDir "StaticProducer.exe"
$CaptureTest = Join-Path $BuildDir "CaptureTest.exe"

Write-Host "=== 1. Building Solution ==="
& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" (Join-Path $Root "VirtualCameraMediaSource.sln") /p:Configuration=Release /p:Platform=x64 /m /v:minimal /nologo

if ($LASTEXITCODE -ne 0) {
    Write-Error "Build failed!"
    exit 1
}

Write-Host "=== 2. Registering MediaSource.dll ==="
& "C:\Windows\System32\regsvr32.exe" /s $MediaDll

Write-Host "=== 3. Preparing Test Image ==="
$TestImage = Join-Path $Root "test_input.bmp"
if (-not (Test-Path $TestImage)) {
    $sampleBmp = Get-ChildItem "C:\Users\Semen\source\repos\10_000.bmp" -ErrorAction SilentlyContinue
    if ($sampleBmp) {
        Copy-Item $sampleBmp.FullName $TestImage
    } else {
        throw "Test image not found!"
    }
}

Write-Host "=== 4. Starting StaticProducer ==="
$ProducerProc = Start-Process -FilePath $StaticProducer -ArgumentList "`"$TestImage`"" -PassThru
Start-Sleep -Seconds 2

Write-Host "=== 5. Running CaptureTest (Direct Mode E2E Verification) ==="
$CaptureOutDir = Join-Path $Root "e2e_output"
if (-not (Test-Path $CaptureOutDir)) { New-Item -ItemType Directory -Path $CaptureOutDir | Out-Null }

$CaptureLog = Join-Path $CaptureOutDir "capture.log"
& $CaptureTest 5 (Join-Path $CaptureOutDir "e2e_frame") 2>&1 | Tee-Object -FilePath $CaptureLog

$CapturedFiles = Get-ChildItem (Join-Path $CaptureOutDir "e2e_frame_*.bmp")
Write-Host "Captured BMP files count: $($CapturedFiles.Count)"

Write-Host "=== 6. Cleanup ==="
if ($ProducerProc -and -not $ProducerProc.HasExited) {
    Stop-Process -Id $ProducerProc.Id -Force -ErrorAction SilentlyContinue
}

if ($CapturedFiles.Count -gt 0) {
    Write-Host "SUCCESS: E2E test passed, frames successfully captured from static image!" -ForegroundColor Green
    exit 0
} else {
    Write-Error "FAILURE: No frames captured!"
    exit 1
}
