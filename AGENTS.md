# VCam — виртуальная камера Windows

Виртуальная камера (Media Foundation): in-proc COM DLL + shared-memory
пайплайн продьюсеров кадров. Весь код — в `VirtualCameraMediaSource/`
(там же `.sln`, `README.md`).

## Структура (`VirtualCameraMediaSource/src/`)

| Каталог | Что |
|---|---|
| `MediaSource/` | Камера (in-proc COM DLL). Регистрация: `build\x64\Release\Registrar.exe add VCam hold` (elevated, процесс не закрывать); страницу «Камеры» Windows Settings перезапускать. |
| `Common/` | Контракты: `SharedMemoryContract.h` (секция `Global\VCam.FrameBuffer.v1`, 1280×720 BGRX, stride 5120, 8 слотов seqlock — **не менять без согласования**), `ProducerApi.h` (`IFrameSource`/`SourceConfig`/`CreateSource`), `SharedMemoryFrameSource` (чтение камеры). |
| `ProducerCore/` | Библиотека источников: `StaticImageSource`, `VideoFileSource`, `FrameWriter` (единственный писатель, 30 FPS pacing, `FlushLast` для hot-switch), `Settings` (новая схема + миграция legacy), `SettingsWatcher` (hot-reload), `SourceFactory`, `TraySourceMenu` (подменю «Источник» + `ApplySourceSwitch` для трей-меню). |
| `VCamVideoStreamProducer/` | Основной tray-хост: single-instance (`VCamVideoStreamProducer.Instance`), Stop-event, автозапуск через задачу Task Scheduler `VCamHost` по `settings.autostart`, hot-switch static↔video (в т.ч. подменю «Источник» в трее), fallback NO SIGNAL. |
| `VCamProducerCli/` | Консольный хост отладки/e2e: `run [--type --path --settings]`, `status [--json\|--ready]`, `diag`, `list-devices`, `list-controls`/`get-control`/`set-control`. |
| `VCamPreview/` | Плавающее окно предпросмотра (GDI+ HighQualityBicubic, single-instance `VCamPreview.Instance`). |
| `VCamSettingsUi/` | C# WinForms настройки (выбор источника, crop, превью, кнопки запуска хоста; single-instance `VCamSettingsUi.Instance`). |
| `CaptureTest/` | Диагностика: захват кадров камеры в BMP (`inspect`, `device`, direct `<n> <prefix>`). |
| `VCamTests/` | Doctest-юниты C++ (сборка только через sln; bin в `src\VCamTests\build\`, фильтр `-tc="..."`). |
| `VCamUiTests/` | Постоянные UIA-тесты UI (FlaUI+xUnit): `dotnet test src\VCamUiTests\VCamUiTests.csproj -c Release`. |
| `ProducerTest/` | Анимированный test pattern → общая память. |
| `StaticProducer/`, `VideoProducer/` | Legacy-утилиты (оставлены для отладки; для работы — хост или CLI). |

## Сборка

```bat
"C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" VirtualCameraMediaSource.sln /p:Configuration=Release /p:Platform=x64 /m /v:m /nologo
```

- exit 0 = успех; csproj (`VCamSettingsUi`) — сначала `dotnet restore`.
- Путь MSBuild выше — VS 18 Community; при другой редакции/версии VS найти
  через `vswhere.exe` (как делает `e2e_test.ps1`, функция `Find-MSBuild`).
- MSB3027 (locked exe) — закрыть запущенные exe проекта перед сборкой.
  Правило останова (05.10.2026, по слову пользователя «останавливай сам»):
  агент ВПРАВЕ сам гасить процессы (`Stop-Process -Name VCamSettingsUi[,…]
  -Force`), но сначала предупреждает, что несохранённые правки в UI сгорят;
  без явного разрешения — только просить закрыть руками.
- **LNK1104 (лочит build-DLL)** — Registrar-holder (`add VCam hold`) грузит build-копию MediaSource.dll → kill holder → build → re-add hold.
- `/utf-8` для всех C++ задано в `Directory.Build.targets` (кириллица в
  wide-литералах безопасна; без него MSVC читал UTF-8 как CP1251 → кракозябры).

## Тесты

- **E2E**: `powershell -ExecutionPolicy Bypass -File e2e_test.ps1` (из
  `VirtualCameraMediaSource/`), exit 0 = SUCCESS. Перед прогоном остановить
  tray-хост — два писателя интерливят кадры. Вся эмпирика (фазы A/B, ассерты,
  питфолы, ручная диагностика): skill **vcam-e2e**
  (`.agents/skills/vcam-e2e/SKILL.md`) — читать его, а не пересматривать скрипт.
- **Юниты**: `VCamTests` (doctest C++) — сборка только через sln, bin в
  `src\VCamTests\build\`; `VCamUiTests` (FlaUI+xUnit) —
  `dotnet test src\VCamUiTests\VCamUiTests.csproj -c Release`.

## Настройки и среда

- `%APPDATA%\VCam\settings.json` — новая схема
  `{"source":{"type":"static|video|camera"},"static":{...},"video":{...},"camera":{...},"quality":...,"hotkey":{...},"recordHotkey":{...},"record":{...},"autostart":bool}`;
  пишет UI и e2e, читают хост и CLI; **UTF-8 без BOM**
  (`Set-Content -Encoding UTF8` даёт BOM — не использовать); файл не блокируется,
  изменения подхватываются на лету.
- Fallback при остановке продьюсера — кэш последнего кадра в течение 7 с, затем NO SIGNAL.

## Критичные питфолы

### MediaSource.dll / COM
- **ThreadingModel=Both обязателен** в HKLM\SOFTWARE\Classes\CLSID\InprocServer32 — иначе MTA-клиент получает кросспартментный COM-прокси, QI IMFMediaSource2 → E_NOINTERFACE, кадры не идут (0xC00D3E9B = MF_E_MEDIA_SOURCE_WRONGSTATE, НЕ SHUTDOWN).
- **Деплой MediaSource.dll** — только через ритуал FrameServer: `sc stop FrameServer` → copy → `sc start FrameServer` (иначе старый DLL в svchost).
- **MF API**: IMFSourceReader не имеет GetStreamCount/SetPosition; IMFMediaType — только SetGUID (нет SetMajorType/SetSubtype); `MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING=TRUE` обязателен для RGB32.
- **CComPtr**: `#include <cguid.h>` после windows.h (INITGUID не даёт GUID_NULL); порядок Release в Shutdown/Close критичен — `= nullptr` до MFShutdown/CoUninitialize.

### Камера / захват
- **trySet S_OK ≠ итоговый формат** — верить только GetCurrentMediaType + ReadSample (конкурентный потребитель может залочить пин).
- **MFCreateSourceReaderFromURL(symlink) → 0x80070002** — рабочий путь: ActivateObject + MFCreateSourceReaderFromMediaSource.
- **MF symlink** читается из `MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK`
  ({58F0AAD8}), НЕ из VIDCAP_GUID.
- **LOCAL SERVICE не может писать в обычные temp-подпапки** — `%TEMP%\VCam\`,
  `%WINDIR%\Temp\VCam\` и файлы, созданные юзером в `%WINDIR%\Temp`, ему
  недоступны (наследованный ACL = ReadAndExecute / нет ACE) → строки
  device-сессий молча терялись (проверено 05.10.2026). Единый diag-sink —
  `%ProgramData%\VCam\msrc_diag.log`: ACL (LOCAL SERVICE / NETWORK SERVICE /
  Users = Modify) выдаёт инсталлятор (`[Run]` icacls) или elevated deploy-ритуал;
  fallback — `<GetTempPath>\VCam\msrc_diag.log` (свой контекст).
- **NV12-конверт**: stride натива может быть с паддингом; тик 0x100 (NATIVEMEDIATYPECHANGED) — не молчание, нужен requery.
- **Прокси контролов камеры**: `E_PROP_ID_UNSUPPORTED` (0x8007490) — нормализованный ответ; отдельный pipe-клиент (MediaSource.vcxproj не линкует ProducerCore).

### Запись / Mp4Recorder
- **MF RGB32 = bottom-up DIB** — при копировании в MF-буфер строки флипаются (иначе видео вверх ногами).
- **Finalize обязателен** — иначе битый mp4; стопать запись ДО выхода worker'а.
- **SinkWriter игнорирует MF_MT_DEFAULT_STRIDE** — конвертер не применяет атрибут, только явный флип.

### Эффекты / GPU
- **GPUPixel**: рантайм /MT (общий CRT); пребилд отвергнут (AV в nvoglv64); патч define.h (GPUPIXEL_STATIC_LINK → пустой API); апстрим-патч шейдеров (highp/lowp/mediump удалять для десктопа).
- **frei0r**: требует w/h кратных 8; swap R↔B для RGBA-плагинов; scanline0r — BGRA (без swap); glitch0r/rgbnoise — глобальный rand() под мьютексом.
- **Beauty-фильтр GPUPixel** отложен — нужны res/lookup_*.png + SetResourcePath, без них AV вне SEH.

### UI / .NET
- **.NET 10**: `Environment.GetFolderPath(ApplicationData)` не слушает env `APPDATA` — редирект не работает; test-seam `filePath` в Settings.Load/Save.
- **PowerShell 5.1**: `.ps1` с кириллицей писать UTF-8 с BOM; `FindWindowW(cls, $null)` передаёт "" не NULL; `Process.ExitCode` пуст при `-RedirectStandardOutput` — обход через Win32-наблюдатель.
- **C# pipe**: `NamedPipeClientStream` не поддерживает ReadTimeout/WriteTimeout — дедлайн через ReadAsync+Wait; CS0177 — `&&` в expression-bodied с out-параметрами запрещён; WFO1000 — публичный setter на UserControl требует DesignerSerializationVisibility(Hidden).

### Профили / настройки
- **Профили = полные снапшоты** — Apply переписывает settings.json побайтово копией профиля (File.Copy); EnsureSeeded встраивает живые source-секции.
- **Сиды с пустым static.path → NO SIGNAL** — честный снапшот; UI показывает warn-хинт.
- **Transient-файлы** (%APPDATA%\VCam\): `record_command.json`, `record_state.json` (stale удаляет стартовый worker), `hotkey_state.json`, `failopen_counters.json`, `runtime_counters.json` (просто перезаписываются), `host_runstate.json` (существует = прошлый выход грязный/краш, маркер в логе; удаляется только при чистом выходе).

### Установщик
- **Версия 0.0.3**; нумерация «по порядку релизов»; фикс «после ребута нет сигнала» — writer-цепочка Create(Global) → Open(Global) → Create(Local).
- **Ярлык без окон** — `wscript.exe` + `vcam_run_host.vbs` (powershell -WindowStyle Hidden мелькает).

### E2E
- **Registrar жив перед прогоном** — иначе inspect count=1, фазы D/E падают «not enumerated».
- **HKCU-приоритет** — bogus-путь → 0x8007007E (HKCU выигрывает у HKLM).
- **CaptureTest device-индекс** — в VM = 0 (одна камера); на хосте = 1.
- **E2E матчит подстроки логов CLI** — `writer ready`, `[cli] switch:`, `[cli] active:`, `frames are being written`; переименование лог-строк ломает фазы (проверено: молчание `SetTarget` роняло B/H).
- **Фаза B flaky** — `frames differ (unique=2), static source expected`: захват стартовал до применения hot-switch после перезаписи settings.json (гонка, не регрессия) → перепрогнать.
- **«installed DLL is STALE»** — после пересборки MediaSource.dll e2e даёт exit 1 и D/E SKIP: guard деплоя, не регрессия → обновить установленную копию (ритуал FrameServer) или дождаться деплоя.

### Превью
- **GDI+ HighQualityBicubic** — MAE 0.023 к эталону (nearest даёт 7.174); ~11 мс/кадр (бюджет 4 мс превышен, кадры не срываются).

## Конвенции

- Ответы по-русски; код/SQL/deploy/коммит — только после явного разрешения
  (см. глобальный `AGENTS.md`).
- Изолируемую работу отдавать субагентам (лимит 2 раунда) с памятью
  вышестоящей задачи в промпте.

## Бэклог

Идеи и планы — в `VirtualCameraMediaSource/backlog/` (по одному .md на тему):
`pipeline-first.md`, `observability.md`, `regression-safety.md`, `testing.md`.
Новые темы: `audio-mix.md`, `drag-drop-source.md`, `tray-source-switch.md`.
