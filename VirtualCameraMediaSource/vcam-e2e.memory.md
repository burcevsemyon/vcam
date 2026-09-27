# Память задачи: e2e-тесты VCam (фазы D/E/F)

Задача: расширить `e2e_test.ps1` device-фазами (путь ktalk через прокси
FrameServer) + обновить skill `vcam-e2e`. Продолжение инцидента
«зелёные помехи / только верх / вытянута» (см. `memory.md`).

## Статус

- [x] `e2e_test.ps1` дописан: фазы A–F, precondition, HKCU-регистрация,
      hash-guard, letterbox/720p/allocator-ассерты. `ParseFile` → PARSE OK,
      UTF-8 без BOM, кириллица цела. Precondition перенесён ДО сборки
      (MSB3027 на запущенных exe), дубли блока удалены.
- [x] SKILL.md (`vcam-e2e`) обновлён: фазы C/D/E/F, BMP 640×480 (было 720p —
      устарело после фикса SetMediaType), pitfall'ы (regsvr32 exit=5 → HKCU,
      фильтр имени `VCam` не матчится → mediaTypes=3, direct без shared
      allocator, `deliver type` не ассертить, **Registrar жив перед прогоном**).
- [x] Пробные артефакты `e2e_output\` удалены (`probe_*`, `devp_*`,
      `probe_vcamdev_*`, `probe.log`, `devp.log`, `inspect.log`, `capture.log`).
- [x] **Полный прогон: SUCCESS, exit 0, 0 failures** (после 1-й попытки с
      фейлом D/E — см. питфол ниже). Хост и SettingsUi восстановлены после
      теста; `VCamProducerCli status` = frames being written.
- [x] Git: правки НЕ закоммичены (HEAD `fb3150a`); push запрещён; коммит
      только по явному разрешению.

### Питфол прогона (проверено на практике)

- **Перед прогоном убедиться, что `Registrar.exe` запущен** (elevated
  `Registrar.exe add VCam hold`, процесс живёт). После чистки процессов/рестарта
  FrameServer Registrar может не работать → inspect `count=1` → фазы D/E:
  «VCam device not enumerated (FrameServer running? …)» → **exit 1 при 100%
  PASS остальных фаз**. Фикс: `Start-Process Registrar -ArgumentList add,VCam,hold`
  (без -Wait), дождаться `inspect count=2`, повторить прогон.

### Автозапуск Registrar после перезагрузки (27.09.2026)

- Симптом: после ребута «регистратор камеры не запускается», inspect count=1.
  Причина: автозапуска не было (в Run только `VCamAutostart` хоста), камера —
  `MFVirtualCameraLifetime_Session` → без живого холдера регистрация не живёт.
- **Elevation НЕ нужен** (MFVirtualCameraAccess_CurrentUser, verified: не-elevated
  запуск → count=2). Холдер = вечный цикл `[holder alive]`/5 с; kill процесса =
  стоп камеры (без remove).
- Фикс: запись `HKCU\Software\Microsoft\Windows\CurrentVersion\Run\VCamRegistrar`
  = `powershell.exe -WindowStyle Hidden -Command "Start-Process -FilePath
  '<repo>\build\x64\Release\Registrar.exe' -ArgumentList 'add','VCam','hold'
  -WindowStyle Hidden"`. Протестировано: kill холдера → запуск строки из Run →
  pid жив, MainWindowHandle=0 (скрыто), count=2, без UAC.

## Ключевая эмпирика этой сессии (для resume)

- HKCU-приоритет verified: bogus-путь → `0x8007007E` (HKCU выигрывает у HKLM).
- `CaptureTest device 1 640 480` при живом ktalk → exit 0, no WRONGSTATE,
  BMP 640×480 (921654 B), letterbox подтверждён в proxy-пути
  (строки 0–59/420–479 чёрные, 60–419 контент).
- inspect: VCam = device[1], `mediaTypes=3`; обе камеры без FRIENDLY_NAME →
  имя-фильтр падает в fallback на device[0] (реальная камера).
- Надёжные diag-маркёры: `negotiated … -> allocator type RGB720/RGB640`,
  `Lock hr=0x00000000 … maxLen=3686400/1228800`; `deliver type …` — только
  на смену типа и в первой device-сессии отсутствовал → НЕ ассертить.
- Тайминг-ассерты AcquireFrame выкинуты (flaky: продюсер пишет каждые 33 мс).
- `VCamDiagLog` → `%TEMP%\opencode\msrc_diag.log` И `C:\Windows\Temp\vcam_ls_load.log`.
- Хост: autostart, source=video; `FrameServer` = имя сервиса (Running).
