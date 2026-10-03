# Сборка GPUPixel static /MT для VCam (разовый скрипт на машине разработчика).
#
# Запуск: powershell -ExecutionPolicy Bypass -File third_party/gpupixel/build.ps1
#   (из каталога VirtualCameraMediaSource; повторный запуск = чистая пересборка)
#
# Что делает:
#   1. Клонирует https://github.com/pixpark/gpupixel во временную папку
#      (%TEMP%\gpupixel-vcam-build, НЕ в репо) на пин-коммит ef552bf8.
#   2. Конфигурирует и собирает x64 Release static /MT (тот же рантайм, что весь VCam).
#   3. Синхронизирует публичные хедеры в third_party/gpupixel/include + применяет
#      локальный патч gpupixel_define.h (GPUPIXEL_STATIC_LINK => пустой API).
#   4. Кладёт gpupixel.lib + glfw3.lib + yuv.lib + glad.lib в lib/x64/
#      (раскладку, на которую ссылаются vcxproj, НЕ меняет).
#
# Флаги (из vcam-effects-gpu.memory.md, проверены сборкой + dumpbin LIBCMT):
#   -DGPUPIXEL_BUILD_SHARED_LIBS=OFF -DGPUPIXEL_ENABLE_FACE_DETECTOR=OFF
#   -DGPUPIXEL_BUILD_DESKTOP_DEMO=OFF -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded
#   "-DCMAKE_CXX_FLAGS=/DBUILDING_GPUPIXEL_DLL" (иначе C2491: апстрим не умеет
#   static-on-Windows). Плюс патч src/CMakeLists.txt ниже: апстрим жёстко ставит
#   MultiThreadedDLL (/MD) — заменяем на MultiThreaded (/MT).
$ErrorActionPreference = "Stop"

$PinCommit = "ef552bf8ce2d0d41fa9b979bfb5c7cf79374ca88"
$RepoUrl = "https://github.com/pixpark/gpupixel"
$WorkDir = Join-Path $env:TEMP "gpupixel-vcam-build"
$ThirdPartyDir = $PSScriptRoot
$LibOutDir = Join-Path $ThirdPartyDir "lib\x64"
$IncludeDir = Join-Path $ThirdPartyDir "include"
$Sw = [System.Diagnostics.Stopwatch]::StartNew()

function Fail([string]$msg) { Write-Host "ОШИБКА: $msg" -ForegroundColor Red; exit 1 }

# --- 0. Проверки окружения ---------------------------------------------------
$git = Get-Command git -ErrorAction SilentlyContinue
if (-not $git) { Fail "нет git. Поставь Git for Windows (https://git-scm.com/download/win) и перезапусти скрипт." }

$VsCmake = "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
$cmake = $null
$cmakeCmd = Get-Command cmake -ErrorAction SilentlyContinue
if ($cmakeCmd) { $cmake = $cmakeCmd.Source }
elseif (Test-Path $VsCmake) { $cmake = $VsCmake }
else { Fail "нет cmake. Варианты: установить VS 2022+ с workload 'Desktop C++' (там есть VS-bundled cmake) или cmake с https://cmake.org/download/ ." }

Write-Host "cmake: $cmake"
& $cmake --version | Select-Object -First 1

# --- 1. Клон на пин-коммит (во временную папку, НЕ в репо) -------------------
if (Test-Path $WorkDir) {
    Write-Host "Чищу $WorkDir (идемпотентность: повторный запуск = пересборка)..."
    Remove-Item -Recurse -Force $WorkDir
}
Write-Host "Клонирую $RepoUrl (shallow)..."
& git clone --depth 1 $RepoUrl $WorkDir
if ($LASTEXITCODE -ne 0) { Fail "git clone не удался (нет сети или нет доступа к github.com)." }
Push-Location $WorkDir
try {
    & git fetch --depth 1 origin $PinCommit
    if ($LASTEXITCODE -ne 0) { Fail "не могу получить пин-коммит $PinCommit (проверь сеть)." }
    & git checkout $PinCommit
    if ($LASTEXITCODE -ne 0) { Fail "checkout $PinCommit не удался." }
    $got = (& git rev-parse HEAD).Trim()
    if ($got -ne $PinCommit) { Fail "ожидался $PinCommit, а вышло $got." }
    Write-Host "Исходники на пине: $got"
} finally { Pop-Location }

# --- 2. Патч рантайма: апстрим форсит /MD, нам нужен /MT ----------------------
$CmakeLists = Join-Path $WorkDir "src\CMakeLists.txt"
$txt = [IO.File]::ReadAllText($CmakeLists)
if ($txt -match "MultiThreadedDLL") {
    $txt = $txt -replace "MultiThreadedDLL", "MultiThreaded"
    [IO.File]::WriteAllText($CmakeLists, $txt)
    Write-Host "Патч src/CMakeLists.txt: MultiThreadedDLL -> MultiThreaded (/MT)."
} else {
    Write-Host "ПРЕДУПРЕЖДЕНИЕ: в src/CMakeLists.txt нет MultiThreadedDLL — /MT держится флагом -DCMAKE_MSVC_RUNTIME_LIBRARY." -ForegroundColor Yellow
}

# --- 3. Конфигурация + сборка --------------------------------------------------
$BuildDir = Join-Path $WorkDir "build"
$generators = @("Visual Studio 18 2026", "Visual Studio 17 2022")
$configured = $false
foreach ($g in $generators) {
    Write-Host "Конфигурирую ($g, x64, Release, static /MT)..."
    & $cmake -S $WorkDir -B $BuildDir -G $g -A x64 `
        -DCMAKE_BUILD_TYPE=Release `
        -DGPUPIXEL_BUILD_SHARED_LIBS=OFF `
        -DGPUPIXEL_ENABLE_FACE_DETECTOR=OFF `
        -DGPUPIXEL_BUILD_DESKTOP_DEMO=OFF `
        -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded `
        "-DCMAKE_CXX_FLAGS=/DBUILDING_GPUPIXEL_DLL"
    if ($LASTEXITCODE -eq 0) { $configured = $true; Write-Host "Генератор: $g"; break }
    Write-Host "Генератор $g не подошёл, пробую следующий..." -ForegroundColor Yellow
    Remove-Item -Recurse -Force $BuildDir -ErrorAction SilentlyContinue
}
if (-not $configured) { Fail "конфигурация cmake не удалась. Нужен VS 2022+ с workload 'Desktop C++' (MSVC x64)." }

Write-Host "Собираю (Release, это несколько минут)..."
& $cmake --build $BuildDir --config Release --parallel
if ($LASTEXITCODE -ne 0) { Fail "сборка GPUPixel не удалась (смотри лог выше)." }

# --- 4. Поиск .lib и раскладка в lib/x64 --------------------------------------
function Find-Lib([string]$name) {
    $cands = @(Get-ChildItem $BuildDir -Recurse -File -Filter $name -ErrorAction SilentlyContinue)
    if ($cands.Count -eq 0) { return $null }
    $rel = $cands | Where-Object { $_.FullName -match "Release" } | Select-Object -First 1
    if (-not $rel) { $rel = $cands | Select-Object -First 1 }
    return $rel.FullName
}

New-Item -ItemType Directory -Path $LibOutDir -Force | Out-Null
$need = @{
    "gpupixel.lib" = "gpupixel.lib"
    "glfw3.lib"    = "glfw3.lib"   # CMake-таргет glfw даёт glfw3.lib на Windows
    "yuv.lib"      = "yuv.lib"
    "glad.lib"     = "glad.lib"
}
foreach ($kv in $need.GetEnumerator()) {
    $found = Find-Lib $kv.Value
    if ((-not $found) -and ($kv.Value -eq "glfw3.lib")) { $found = Find-Lib "glfw.lib" } # запасной вариант имени
    if (-not $found) { Fail "не найден $($kv.Value) в $BuildDir (сборка прошла, но артефакта нет)." }
    Copy-Item $found (Join-Path $LibOutDir $kv.Key) -Force
    $mb = ((Get-Item (Join-Path $LibOutDir $kv.Key)).Length / 1MB).ToString("0.0")
    Write-Host "  $($kv.Key) <- $found ($mb MB)"
}

# --- 5. Хедеры + локальный патч gpupixel_define.h ------------------------------
$SrcInc = Join-Path $WorkDir "include\gpupixel"
$DstInc = Join-Path $IncludeDir "gpupixel"
if (-not (Test-Path $SrcInc)) { Fail "в исходниках нет include/gpupixel (структура апстрима изменилась)." }
if (Test-Path $DstInc) { Remove-Item -Recurse -Force $DstInc }
New-Item -ItemType Directory -Path $DstInc -Force | Out-Null
Copy-Item (Join-Path $SrcInc "*") $DstInc -Recurse -Force
Write-Host "Хедеры синхронизированы: include/gpupixel."

$DefineH = Join-Path $DstInc "gpupixel_define.h"
$dh = [IO.File]::ReadAllText($DefineH)
if ($dh -notmatch "GPUPIXEL_STATIC_LINK") {
    $oldBlock = "#ifdef _WIN32`r?`n#ifdef BUILDING_GPUPIXEL_DLL"
    if ($dh -notmatch "#ifdef BUILDING_GPUPIXEL_DLL") { Fail "неожиданный формат gpupixel_define.h (нет BUILDING_GPUPIXEL_DLL) — патч вручную." }
    $dh = $dh -replace $oldBlock, "#ifdef _WIN32`r`n#ifdef GPUPIXEL_STATIC_LINK`r`n// VCam: линкуем static lib (/MT) — никакого dllimport/dllexport.`r`n#define GPUPIXEL_API`r`n#elif defined(BUILDING_GPUPIXEL_DLL)"
    [IO.File]::WriteAllText($DefineH, $dh)
    Write-Host "Патч gpupixel_define.h применён (GPUPIXEL_STATIC_LINK => пустой API)."
} else {
    Write-Host "Патч gpupixel_define.h уже на месте."
}
$check = [IO.File]::ReadAllText($DefineH)
if ($check -notmatch "GPUPIXEL_STATIC_LINK") { Fail "патч gpupixel_define.h не применился." }

# --- 6. Проверка /MT (предупреждение, не фатально) ------------------------------
$dumpbin = $null
foreach ($msvc in @(Get-ChildItem "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Tools\MSVC" -Directory -ErrorAction SilentlyContinue | Sort-Object Name -Descending)) {
    $cand = Join-Path $msvc.FullName "bin\Hostx64\x64\dumpbin.exe"
    if (Test-Path $cand) { $dumpbin = $cand; break }
}
if ($dumpbin) {
    $dirs = & $dumpbin /DIRECTIVES (Join-Path $LibOutDir "gpupixel.lib") | Select-String "DEFAULTLIB" | Select-Object -ExpandProperty Line -First 20
    if (($dirs -match "MSVCRT") -and (-not ($dirs -match "LIBCMT"))) {
        Write-Host "ПРЕДУПРЕЖДЕНИЕ: gpupixel.lib тянет MSVCRT (/MD?), ожидался LIBCMT (/MT)." -ForegroundColor Yellow
    } else {
        Write-Host "/MT проверен (LIBCMT в gpupixel.lib)."
    }
} else {
    Write-Host "dumpbin не найден — проверку /MT пропускаю." -ForegroundColor Yellow
}

$totalMb = ((Get-ChildItem $LibOutDir *.lib | Measure-Object Length -Sum).Sum / 1MB).ToString("0.0")
Write-Host "ГОТОВО за $([int]$Sw.Elapsed.TotalSeconds) с: $LibOutDir ($totalMb MB, 4 x .lib). Хедеры + NOTICE.txt остаются в git, .lib — игнорируются." -ForegroundColor Green
