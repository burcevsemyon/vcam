# Сборка frei0r-плагинов VCam (помехи аналогового сигнала, backend "frei0r").
#
# Запуск: powershell -ExecutionPolicy Bypass -File third_party/frei0r/build.ps1
#   (из каталога VirtualCameraMediaSource; повторный запуск = чистая пересборка)
#
# Почему не готовые .dll: у dyne/frei0r нет официальных Win64-бинарников
# (releases — только исходники), поэтому собираем 4 нужных фильтра из
# исходников апстрима клангом MSVC:
#   rgbnoise   (шум)          <- src/filter/rgbnoise/rgbnoise.c
#   scanline0r (сканлайны)    <- src/filter/scanline0r/scanline0r.cpp
#   rgbsplit0r (RGB-сдвиг)    <- src/filter/rgbsplit0r/rgbsplit0r.c
#   glitch0r   (трекинг-глитч)<- src/filter/glitch0r/glitch0r.c
#
# Что делает:
#   1. Клонирует https://github.com/dyne/frei0r во временную папку
#      (%TEMP%\frei0r-vcam-build, НЕ в репо) на пин-коммит $PinCommit.
#   2. Забирает include/frei0r.h, include/frei0r.hpp, include/frei0r/math.h
#      + 4 исходника в $WorkDir\src (временно, НЕ в репо).
#   3. Компилирует каждый в DLL: cl /O2 /MT /LD /DFREI0R_PLUGIN + сгенерённый
#      .def (апстрим не ставит dllexport на определения f0r_* — без .def
#      экспортов в DLL не будет; проверено dumpbin).
#   4. Кладёт rgbnoise.dll, scanline0r.dll, rgbsplit0r.dll, glitch0r.dll в
#      third_party/frei0r/bin/x64/ (игнор) + копию в build\x64\Release\frei0r\
#      (рядом с хостом; build/ в игноре).
#
# Лицензия: плагины — GPL-2.0 (апстрим); DLL НЕ коммитятся, в инсталлятор
# попадут как внешние бинарники с указанием исходников (см. NOTICE.txt).
# Наш хост грузит их только динамически (LoadLibrary) с fail-open на CPU.
$ErrorActionPreference = "Stop"

$PinCommit = "5378516e73500c2d58a975eb85a305b3150475f3"
$RepoUrl = "https://github.com/dyne/frei0r"
$WorkDir = Join-Path $env:TEMP "frei0r-vcam-build"
$ThirdPartyDir = $PSScriptRoot
$BinOutDir = Join-Path $ThirdPartyDir "bin\x64"
$RepoRoot = Split-Path (Split-Path $ThirdPartyDir -Parent) -Parent
$BuildFreiDir = Join-Path $RepoRoot "build\x64\Release\frei0r"
$Sw = [System.Diagnostics.Stopwatch]::StartNew()

function Fail([string]$msg) { Write-Host "ОШИБКА: $msg" -ForegroundColor Red; exit 1 }

# --- 0. Проверки окружения ---------------------------------------------------
$git = Get-Command git -ErrorAction SilentlyContinue
if (-not $git) { Fail "нет git. Поставь Git for Windows и перезапусти скрипт." }

$VcVars = "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
if (-not (Test-Path $VcVars)) { Fail "нет vcvars64.bat ($VcVars). Нужен VS 2022+ с workload 'Desktop C++'." }

# --- 1. Клон на пин-коммит (во временную папку, НЕ в репо) -------------------
if (Test-Path $WorkDir) {
    Write-Host "Чищу $WorkDir (идемпотентность: повторный запуск = пересборка)..."
    Remove-Item -Recurse -Force $WorkDir
}
Write-Host "Клонирую $RepoUrl (shallow)..."
& git clone --depth 1 $RepoUrl (Join-Path $WorkDir "repo")
if ($LASTEXITCODE -ne 0) { Fail "git clone не удался (нет сети или нет доступа к github.com)." }
$RepoDir = Join-Path $WorkDir "repo"
Push-Location $RepoDir
try {
    & git fetch --depth 1 origin $PinCommit
    if ($LASTEXITCODE -ne 0) { Fail "не могу получить пин-коммит $PinCommit (проверь сеть)." }
    & git checkout $PinCommit
    if ($LASTEXITCODE -ne 0) { Fail "checkout $PinCommit не удался." }
    $got = (& git rev-parse HEAD).Trim()
    if ($got -ne $PinCommit) { Fail "ожидался $PinCommit, а вышло $got." }
    Write-Host "Исходники на пине: $got"
} finally { Pop-Location }

# --- 2. Забор нужных файлов --------------------------------------------------
$SrcDir = Join-Path $WorkDir "src"
New-Item -ItemType Directory -Path (Join-Path $SrcDir "inc\frei0r") -Force | Out-Null
$IncDir = Join-Path $SrcDir "inc"
foreach ($h in @("include\frei0r.h", "include\frei0r.hpp", "include\frei0r\math.h")) {
    $from = Join-Path $RepoDir $h
    if (-not (Test-Path $from)) { Fail "в исходниках нет $h (структура апстрима изменилась)." }
    $to = Join-Path $IncDir (Split-Path $h -Leaf)
    if ($h -match "math\.h$") { $to = Join-Path $IncDir "frei0r\math.h" }
    Copy-Item $from $to -Force
}
$Plugins = @(
    @{ Name = "rgbnoise";   Src = "src\filter\rgbnoise\rgbnoise.c";       Lang = "c" },
    @{ Name = "scanline0r"; Src = "src\filter\scanline0r\scanline0r.cpp"; Lang = "cpp" },
    @{ Name = "rgbsplit0r"; Src = "src\filter\rgbsplit0r\rgbsplit0r.c";   Lang = "c" },
    @{ Name = "glitch0r";   Src = "src\filter\glitch0r\glitch0r.c";       Lang = "c" }
)
foreach ($p in $Plugins) {
    $from = Join-Path $RepoDir $p.Src
    if (-not (Test-Path $from)) { Fail "в исходниках нет $($p.Src) (структура апстрима изменилась)." }
    Copy-Item $from (Join-Path $SrcDir "$($p.Name).$($p.Lang)") -Force
    Write-Host "  $($p.Name) <- $($p.Src)"
}

# --- 3. Компиляция каждого плагина в DLL -------------------------------------
# Тонкости апстрима (проверены сборкой):
# - определения f0r_* без __declspec(dllexport) → MSVC ничего не экспортирует;
#   .def генерируем по факту: dumpbin /symbols объекта показывает, какие f0r_
#   реально определены (C — undecorated, C++ frei0r.hpp — mangled ?f0r_*@@...;
#   для mangled в .def пишем алиас "f0r_x = ?f0r_x@@...").
# - C-плагины (rgbnoise/glitch0r/rgbsplit0r) определяют только f0r_update;
#   недостающий f0r_update2 добиваем C-шимом (форвард на f0r_update) — тогда
#   у всех 4 DLL единый интерфейс (хост предпочитает update2).
$dumpbin = $null
foreach ($msvc in @(Get-ChildItem "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Tools\MSVC" -Directory -ErrorAction SilentlyContinue | Sort-Object Name -Descending)) {
    $cand = Join-Path $msvc.FullName "bin\Hostx64\x64\dumpbin.exe"
    if (Test-Path $cand) { $dumpbin = $cand; break }
}
if (-not $dumpbin) { Fail "нет dumpbin (нужен для генерации .def по символам объекта)." }

$ObjDir = Join-Path $WorkDir "obj"
New-Item -ItemType Directory -Path $ObjDir -Force | Out-Null
New-Item -ItemType Directory -Path $BinOutDir -Force | Out-Null

$ShimC = Join-Path $ObjDir "f0r_update2_shim.c"
Set-Content -Path $ShimC -Value @"
#include <stdint.h>
typedef void* f0r_instance_t;
extern void f0r_update(f0r_instance_t instance, double time, const uint32_t* inframe, uint32_t* outframe);
void f0r_update2(f0r_instance_t instance, double time,
    const uint32_t* inframe1, const uint32_t* inframe2, const uint32_t* inframe3, uint32_t* outframe) {
    (void)inframe2; (void)inframe3;
    f0r_update(instance, time, inframe1, outframe);
}
"@ -Encoding Ascii

$F0rNames = @("f0r_init", "f0r_deinit", "f0r_get_plugin_info", "f0r_get_param_info",
    "f0r_construct", "f0r_destruct", "f0r_set_param_value", "f0r_get_param_value",
    "f0r_update", "f0r_update2")

foreach ($p in $Plugins) {
    $name = $p.Name
    $src = Join-Path $SrcDir "${name}.$($p.Lang)"
    $obj = Join-Path $ObjDir "${name}.obj"
    $def = Join-Path $ObjDir "${name}.def"
    $dll = Join-Path $BinOutDir "${name}.dll"
    $bat = Join-Path $ObjDir "build_${name}.bat"
    # Шаг 1: compile-only.
    $step1 = "@echo off`r`ncall `"$VcVars`" >nul`r`ncl /nologo /O2 /MT /EHsc /c /DFREI0R_PLUGIN /I`"$IncDir`" `"$src`" /Fo`"$obj`"`r`n"
    Set-Content -Path $bat -Value $step1 -Encoding Ascii
    Write-Host "Компилирую $name ..."
    & cmd /c "`"$bat`""
    if ($LASTEXITCODE -ne 0) { Fail "компиляция $name не удалась (смотри лог выше)." }
    # Шаг 2: какие f0r_ определены в объекте (defined = есть SECT, не UNDEF).
    $syms = & $dumpbin /symbols $obj | Out-String
    $exports = @()
    foreach ($fn in $F0rNames) {
        if ($fn -eq "f0r_update2") { continue } # dobьём ниже (шим или mangled)
        $mm = [regex]::Match($syms, "\?" + $fn + "@@[^ \|]+")
        if ($mm.Success) {
            $exports += "    " + $fn + " = " + $mm.Value
            continue
        }
        $ok = $false
        foreach ($ln in ($syms -split "`r?`n")) {
            if ($ln -match "SECT" -and $ln -match "External" -and $ln -match "[ _]$fn(\|| |$)") { $ok = $true; break }
        }
        if ($ok) { $exports += "    " + $fn }
        else { Fail "$name.obj: не найден определённый символ $fn (апстрим изменился?)." }
    }
    $extraObjs = ""
    $hasU2 = $false
    foreach ($ln in ($syms -split "`r?`n")) {
        if ($ln -match "SECT" -and $ln -match "External" -and $ln -match "f0r_update2") { $hasU2 = $true; break }
    }
    if ($hasU2 -and ($syms -split "`r?`n" | Where-Object { $_ -match "SECT" -and $_ -match "External" -and $_ -match "\?f0r_update2@@" } | Select-Object -First 1)) {
        $mangled = [regex]::Match(($syms -split "`r?`n" | Where-Object { $_ -match "\?f0r_update2@@" } | Select-Object -First 1), "\?f0r_update2@@[^ \|]+").Value
        $exports += "    f0r_update2 = $mangled"
        Write-Host "  ${name}: f0r_update2 mangled (C++), алиас в .def."
    } elseif ($hasU2) {
        $exports += "    f0r_update2"
        Write-Host "  ${name}: f0r_update2 уже определён, экспорт напрямую."
    } else {
        # C-плагин без update2: шим-форвард.
        $shimObj = Join-Path $ObjDir "${name}_shim.obj"
        $stepShim = "@echo off`r`ncall `"$VcVars`" >nul`r`ncl /nologo /O2 /MT /EHsc /c `"$ShimC`" /Fo`"$shimObj`"`r`n"
        Set-Content -Path $bat -Value $stepShim -Encoding Ascii
        & cmd /c "`"$bat`""
        if ($LASTEXITCODE -ne 0) { Fail "компиляция шима $name не удалась." }
        $extraObjs = " `"$shimObj`""
        $exports += "    f0r_update2"
        Write-Host "  ${name}: f0r_update2 нет — добавлен шим-форвард на f0r_update."
    }
    # Шаг 3: .def + линковка DLL.
    $defText = "LIBRARY $name`r`nEXPORTS`r`n" + ($exports -join "`r`n") + "`r`n"
    Set-Content -Path $def -Value $defText -Encoding Ascii
    $implib = Join-Path $ObjDir "${name}.lib"
    $step3 = "@echo off`r`ncall `"$VcVars`" >nul`r`nlink /nologo /DLL /DEF:`"$def`" `"$obj`"$extraObjs /OUT:`"$dll`" /IMPLIB:`"$implib`"`r`n"
    Set-Content -Path $bat -Value $step3 -Encoding Ascii
    Write-Host "Линкую ${name}.dll ..."
    & cmd /c "`"$bat`""
    if ($LASTEXITCODE -ne 0) { Fail "линковка ${name}.dll не удалась (смотри лог выше)." }
    if (-not (Test-Path $dll)) { Fail "артефакт ${name}.dll не появился." }
    $mb = ((Get-Item $dll).Length / 1KB).ToString("0")
    Write-Host "  ${name}.dll (${mb} KB)"
}

# --- 4. Проверка экспортов ----------------------------------------------------
$dumpbin = $null
foreach ($msvc in @(Get-ChildItem "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Tools\MSVC" -Directory -ErrorAction SilentlyContinue | Sort-Object Name -Descending)) {
    $cand = Join-Path $msvc.FullName "bin\Hostx64\x64\dumpbin.exe"
    if (Test-Path $cand) { $dumpbin = $cand; break }
}
if ($dumpbin) {
    foreach ($p in $Plugins) {
        $dll = Join-Path $BinOutDir "$($p.Name).dll"
        $exps = & $dumpbin /exports $dll | Select-String "f0r_" | Measure-Object | Select-Object -ExpandProperty Count
        Write-Host "  $($p.Name).dll: f0r_-экспортов = $exps"
        $need = 9
        if ($exps -lt $need) { Fail "$($p.Name).dll: мало f0r_-экспортов: $exps, нужно $need — .def не сработал." }
    }
} else {
    Write-Host "dumpbin не найден — проверку экспортов пропускаю." -ForegroundColor Yellow
}

# --- 5. Копия рядом с хостом ---------------------------------------------------
New-Item -ItemType Directory -Path $BuildFreiDir -Force | Out-Null
Copy-Item (Join-Path $BinOutDir "*.dll") $BuildFreiDir -Force
Write-Host "Копия для хоста: $BuildFreiDir"

$totalKb = ((Get-ChildItem $BinOutDir *.dll | Measure-Object Length -Sum).Sum / 1KB).ToString("0")
Write-Host "ГОТОВО за $([int]$Sw.Elapsed.TotalSeconds) с: $BinOutDir ($totalKb KB, 4 x .dll). В git идут только build.ps1 + NOTICE.txt, .dll — игнорируются." -ForegroundColor Green
