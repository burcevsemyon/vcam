# Архитектура VCam

Документ фиксирует слои, основные сущности, зависимости и план реорганизации
кодовой базы. Источник истины по контрактам — `src/Core/Contracts/SharedMemoryContract.h`
и `src/Core/Contracts/ProducerApi.h` (не менять без согласования).

## 0. Целевая архитектура

**Портово-адаптерная (Hexagonal) поверх слоёв.** Доменная область VCam бедная
(мало сущностей, много инфраструктуры: COM, Media Foundation, GDI+, shared
memory), поэтому Screaming Architecture не подходит — папки были бы пустыми
обёртками вокруг утилит. Вместо этого:

- **Ядро/порт:** контракты (`IFrameSource`, `SourceConfig`, shared memory v1/v2)
  + `FrameWriter`/`PipelineEngine` — маленький доменный центр.
- **Адаптеры:** `MediaSource.dll` (COM-адаптер к shared memory), хосты
  (`VCamVideoStreamProducer`, `VCamProducerCli`), UI (.NET), `CaptureTest`.
- **Направление зависимости:** адаптеры зависят от ядра, не наоборот.

Границы по проектам уже правильные (`Common` ← `ProducerCore` ← хосты;
`MediaSource` изолирован). Hexagonal формализует то, что уже есть.

## 1. Слои

| Слой | Каталоги/проекты | Назначение и граница |
|---|---|---|
| L0. Контракты и утилиты | `src/Core/Contracts` (header-only) | Общий знаменатель C++-модулей: контракты shared memory (v1/v2, seqlock), `IFrameSource`/`SourceConfig`, `SharedMemoryFrameSource` (чтение v1), математика пайплайна, диагностика/логи, GUIDs. Нет зависимостей на другие слои проекта. |
| L1. Ядро источников | `src/Core/Producer` (статическая lib) | Настройки, источники кадров, `FrameWriter`, `PipelineEngine`, скейлер, устройства камера/контролы, `ControlServer`, `TraySourceMenu`, `SourceFactory`. Зависит только от L0. |
| L2. Виртуальная камера (COM DLL) | `src/Adapters/Camera` (`MediaSource.dll`) | COM-источник Media Foundation: `CMediaSource`, `CMediaStream`, активатор, `V2FrameReader`, `ControlProxyClient`. Отдельная DLL, **не линкует** `Producer` (только L0); читает shared memory напрямую. |
| L3. Хосты/утилиты | `VCamVideoStreamProducer`, `VCamProducerCli`, `VCamPreview`, `CaptureTest`, `Registrar`, `ProducerTest` | Трей-хост, CLI, превью, диагностика. Хосты линкуют `ProducerCore`; превью/диагностика используют L0. |
| L4. UI (.NET) | `VCamSettingsUi` (WinForms, .NET 10), `src/Tests/Ui` (FlaUI) | Настройки и UI-тесты; контракт с C++ — `settings.json`. Независимый стек. |
| L5. Тесты | `src/Tests` (doctest), `src/Tests/Ui` | Юнит-тесты C++ и UIA-тесты .NET. |

Ключевая граница: `MediaSource` (L2) не видит `ProducerCore` (L1) — поэтому транспорт контролов и парсеры в `MediaSource` дублируются минимально.

## 2. Основные сущности

### L0. Contracts

- `VCamSectionHeader`, константы v1/v2 — `SharedMemoryContract.h`: frozen-контракт shared memory (1280×720, 8 слотов v1; v2 до 4K, 4 слота, seqlock).
- `IFrameSource` / `SourceConfig` / `CreateSource` — `ProducerApi.h`: абстракция источника кадров BGRX + конфиг.
- `SharedMemoryFrameSource` — `SharedMemoryFrameSource.h/.cpp`: чтение v1-секции камерой, seqlock, кэш 7 с, NO SIGNAL.
- `MappedViewOfFilePtr` — `MappedViewOfFilePtr.h`: RAII для `MapViewOfFile`.
- `CopyFrameRowwise` — `FrameCopy.h`: memcpy по строкам при разных stride.
- `LetterboxNearest/Bilinear/Ex`, `StretchBilinearEx`, `CoverBilinearEx`, `RowPtr` — `ImageLayout.h`: масштабирование/letterbox/cover RGB32.
- `LetterboxNearestFit` — `Letterbox.h`: nearest letterbox-fit.
- `NormalizeQuality`, `ValidateFrameDims`, `ShouldUseNative` — `PipelineLogic.h`: чистая логика пайплайна/FrameWriter.
- `ResolveV2Size` — `QualityLadder.h`: лесенка качества v2.
- `HrHex`, `WinErr` — `WinUtil.h`: единые форматтеры ошибки.
- `InitSectionHeader/V2` — `SectionHeaderInit.h`: инициализация заголовка секции.
- `FormatLogPrefix/Line`, `LogLevel`, `IsDebugEnabled` — `LogFormat.h`: единый формат логов.
- `RotateLogIfOverCap` — `LogRotate.h`: ротация логов по cap.
- `FailOpen*`, `FailOpenCounter` — `FailOpenCounters.h`, `FailOpenFile.h`: счётчики деградаций + персистентность.
- `RuntimeCounters`, `Write/ReadRuntimeCounters` — `RuntimeCounters.h`: runtime-метрики продьюсера.
- `InstallCrashHandler`, `PruneOldDumps` — `CrashDump.h`: минидампы.
- `FormatHotkey` — `HotkeyDisplay.h`: форматирование хоткея.
- `IMFSampleAllocatorControl` (mirror) — `SampleAllocatorControl.h`: локальное зеркало SDK-интерфейса.
- GUIDs (`CLSID_VCamMediaSource` и др.) — `GUIDs.h`.

### L1. Producer

- `Settings` (`Settings.h/.cpp`): схема настроек + парсинг legacy/new + сериализация + `ToSourceConfig`.
- `SettingsWatcher` (`SettingsWatcher.h/.cpp`): poll 500 ms + debounce, hot-reload.
- `FrameWriter` (`FrameWriter.h/.cpp`): единственный писатель v1+v2, pacing 30 FPS, `FlushLast`, кэш.
- `PipelineEngine` (`PipelineEngine.h/.cpp`): оркестрация Switch/Active/Fallback, `SetTarget`, метрики.
- `HostPipelineEngine` (`Adapters/Host/HostPipelineEngine.h/.cpp`): наследник — запись эфира (`Mp4Recorder`), `PostProcessFrame`.
- `CliPipelineEngine` (`Adapters/Cli/CliPipelineEngine.h/.cpp`): наследник — только лог в консоль.
- `StaticImageSource` (`StaticImageSource.h/.cpp`): WIC-загрузка статики (720p + native до 4K), crop/scale.
- `VideoFileSource` (`VideoFileSource.h/.cpp`): MF SourceReader, decode-поток, play/pause, loop/ended, масштабирование.
- `CameraSource` (`CameraSource.h/.cpp`): захват физ. камеры (MF), NV12→RGB32, adaptive native tick, контролы + `ControlServer`.
- `CameraDeviceInfo`, `EnumerateCameraDevices`, `OpenCameraActivate` — `CameraDevices.h/.cpp`.
- `CameraControls` — `CameraControls.h/.cpp`: QI IAM* с устройства, интроспекция/применение свойств.
- `ControlServer` — `ControlServer.h/.cpp`: named pipe `\\.\pipe\VCamControl.v1`, JSON-протокол list/get/set; `ControlClientRequest` — синхронный клиент.
- `TraySourceMenu` — `TraySourceMenu.h/.cpp`: подменю «Источник» + запись `source.type`.
- `VideoProcessorScaler` — `VideoProcessorScaler.h/.cpp`: VP MFT скейлер 720p + CPU-fallback.
- `SourceFactory::CreateSource` — `SourceFactory.cpp`: фабрика `IFrameSource` по типу.
- `CameraSource` inline helpers (`Nv12ToRgb32`, `NativeScoreFor`, `SortIndicesByScore`, …) — `CameraSource.h`.

### L2. Camera (MediaSource)

- `CMediaSource` (`MediaSource.h/.cpp`): COM-источник MF — события, старт/стоп, shutdown, атрибуты, IKsControl, IAM-прокси.
- `CMediaStream` (`MediaStream.h/.cpp`): доставка сэмплов (v1 бит-в-бит + v2), worker thread, NV12/RGB32, letterbox, allocator.
- `CVCamActivator` (`Activator.h/.cpp`): IMFActivate endpoint FrameServer.
- `V2Reader`, `ShouldAdvertiseNative`, `DownscaleRgb32Letterbox`, `IsV2HeaderValid` — `V2FrameReader.h/.cpp`: read-only читатель v2-секции.
- `CProcAmpProxy`, `CCameraProxy`, `VCamProxy*` — `ControlProxyClient.h/.cpp`: IAM-прокси поверх pipe-канала продьюсера.
- `CAttrLogProxy` — `AttrLogProxy.h/.cpp`: TEMP DIAGNOSTIC — логирование доступа к атрибутам.
- `ModuleLifetime` (`ModuleLifetime.h/.cpp`): счётчик объектов DLL.

### L3. Хосты/адаптеры

- `VCamVideoStreamProducer.cpp` + `HostGlobals.h`: single-instance, tray-окно, worker, горячие клавиши, автозапуск.
- `HostTray` (`HostTray.h/.cpp`): трей-меню и реакция на изменение настроек.
- `HostStatus` (`HostStatus.h/.cpp`): статус, дифф настроек, crash-маркер, cleanup.
- `HostHotkey` (`HostHotkey.h/.cpp`): регистрация/обработка хоткеев, borrow-логика.
- `HostRecording` (`HostRecording.h/.cpp`): пути `record_state/command`, парсинг малых JSON, transient-файлы.
- `HostCameraLifecycle` (`HostCameraLifecycle.h/.cpp`): взаимодействие с Registrar-holder.
- `HostAutostart` (`HostAutostart.h/.cpp`): задача Task Scheduler `VCamHost`.
- `HostLogging` (`HostLogging.h/.cpp`): логирование host.log с ротации.
- `HostUtils` (`HostUtils.h/.cpp`): утилиты хоста.
- `Mp4Recorder` (`Mp4Recorder.h/.cpp`): запись эфира в MP4 (SinkWriter, H.264, flip строк).
- `VCamProducerCli` + `CliPipelineEngine`: CLI run/status/diag/list-devices/list-controls.
- `VCamPreview` (`VCamPreview.cpp`): плавающее окно предпросмотра (GDI+, собственный читатель).
- `CaptureTest` (`Tools/Diag/*.cpp`): диагностика захвата камеры.

### L4. .NET

- `Settings` (`Adapters/SettingsUi/Settings.cs`): схема + Load/Save/Migrate (JSON).
- `MainForm`, `CropPreviewControl`, `FullSizeForm`, `HotkeyEditForm`, `HelpForm`/`HelpTexts`, `PreviewRenderer`, `Program` — UI настроек, превью, запуск хоста.
- UIA-тесты (`ModeSwitchTests`, `SettingsQualityTests`, `SettingsSourceHotkeyTests`, `SettingsVideoTests`, `UiAppFixture`) — `src/Tests/Ui`.

## 3. Зависимости между проектами

- `VCamVideoStreamProducer.vcxproj` → `ProducerCore.vcxproj`
- `VCamProducerCli.vcxproj` → `ProducerCore.vcxproj`
- `VCamUiTests.csproj` → `VCamSettingsUi.csproj`
- `VCamTests.vcxproj` не ссылается на `ProducerCore` как проект: компилирует напрямую `..\Producer\Settings.cpp`, `StaticImageSource.cpp`, `TraySourceMenu.cpp` и подключает `..\Contracts`, `..\Producer`, `..\..\..\third_party\doctest` по include-путям.
- `MediaSource.vcxproj`, `CaptureTest.vcxproj`, `VCamPreview.vcxproj`, `Registrar.vcxproj`, `ProducerTest`/`StaticProducer`/`VideoProducer` — самостоятельные проекты; используют `Contracts` через include-пути (без project references).
- `Contracts` — header-only, отдельного `.vcxproj` нет.
- `VCamSettingsUi.csproj` — самостоятельный .NET-проект, без ссылок на C++.

Итог: C++-зависимости — «ромб» через `ProducerCore` (только два хоста); `MediaSource` и утилиты изолированы на уровне проектов, но связаны через заголовки L0 и shared memory.

## 4. Дублирование и нарушения

### 4.1. Парсинг настроек (C++ vs C#)
`src/Core/Producer/Settings.cpp` содержит собственный плоский JSON-парсер; `src/Adapters/SettingsUi/Settings.cs` реализует ту же схему через `System.Text.Json`. Логика миграции legacy, валидация hotkey (1–15 / 0x08–0xFE), quality/capture, scaleMode дублируется. В комментариях уже фиксировались рассинхроны (K4 для `mediaMode`/`scaleMode`).

### 4.2. Чтение shared memory
`SharedMemoryFrameSource` (L0) — канонический читатель v1; `V2Reader` (L2) — отдельный read-only читатель v2; `VCamPreview.cpp` читает v1/v2 секции напрямую; `CaptureTest` имеет собственные сессии MF. Несколько независимых реализаций seqlock/открытия Global→Local.

### 4.3. Масштабирование/letterbox
`Core/Contracts/Letterbox.h`, `Core/Producer/ImageLayout.h`, `Adapters/Camera/V2FrameReader.h` (`DownscaleRgb32Letterbox`), `VCamPreview` — разные реализации; комментарии требуют синхронизации.

### 4.4. Транспорт контролов (архитектурное ограничение)
`ControlServer` + `ControlClientRequest` (L1) и `ControlProxyClient.cpp` (L2) реализуют один и тот же pipe-протокол и свой мини-JSON-парсер, потому что `MediaSource` не линкует `ProducerCore`. `ControlProxyClient.cpp` содержит полный `JParser`/`JVal`, хотя L1-сторона использует ручной парсинг.

### 4.5. Форматтеры/утилиты
- `vcam::HrHex` (L0) vs `Mp4Recorder::HrHex` (private static).
- `vcam::FormatHotkey` (L0) vs `HotkeyDisplay` в `HostHotkey.h`.
- `SettingsFileGuard`: два разных типа с одинаковым именем — в `HostGlobals.h` (guard'ит `g_settingsCs`) и в anonymous namespace `HostPipelineEngine.cpp` (свой `static m_settingsCs`). Потенциальная путаница, какой мьютекс реально защищает файл настроек.

### 4.6. Нарушения SRP
- `CMediaStream`: доставка сэмплов, worker thread, v1/v2 приобретение, NV12→RGB32, letterbox, allocator, логирование per-frame.
- `CameraSource`: захват, NV12-конверт, адаптация native tick, контролы, `ControlServer`, скейлер.
- `VideoFileSource`: decode-поток, seek/play/pause, ended-state, масштабирование.
- `FrameWriter`: v1+v2 секции, pacing, кэш, скейлер, seqlock-публикация.
- `PipelineEngine`: базовый класс с protected-полями и виртуальным `Log`; наследники переопределяют только логирование, `HostPipelineEngine` добавляет запись эфира.
- `Settings` (C++ и C#): структура данных + парсинг + сериалзация + миграция + валидация в одном типе.

### 4.7. Coupling / глобальное состояние
- `HostGlobals.h` экспортирует десятки глобальных переменных (`g_inst`, `g_hwnd`, `g_mutex`, `g_stop`, `g_dirty`, `g_worker`, `g_watcher`, `g_nid`, `g_logFile`, `g_statusCs`, …). Функции в `HostStatus`/`HostHotkey`/`HostTray` работают с ними неявно.
- Single-instance и остановка через именованные объекты (`VCamVideoStreamProducer.Instance`, `.Stop`).
- `CAttrLogProxy` (TEMP DIAGNOSTIC) встроен в production-путь `CMediaSource`/`CMediaStream`.
- `VCamPreview` и `CaptureTest` не переиспользуют L0-читатели.

### 4.8. Подозрительный код (latent bug)
- `ControlProxyClient.cpp::CtlUnsupported()` вызывает саму себя (`return CtlUnsupported();`) вместо возврата `E_PROP_ID_UNSUPPORTED` — рекурсия при любом отказе контрола. Не покрыто тестами (VCamTests не компилирует этот файл).

## 5. План реорганизации (по этапам)

### Этап 1. Документация и контракты (без изменения кода)
1. Зафиксировать этот документ как каноническую карту сущностей.
2. Добавить golden-файлы settings (legacy/new) и тесты на парсинг/сериализацию в обоих стеках.
3. Устранить `CtlUnsupported()` (рекурсия) — возврат `E_PROP_ID_UNSUPPORTED`.

### Этап 2. Консолидация L0
4. Единый читатель shared memory (`SharedMemoryReader` v1+v2) с seqlock/Global→Local и кэшем; перевести `MediaSource`, `VCamPreview`, `CaptureTest`.
5. Единый набор функций масштабирования/letterbox в L0; убрать копии в `V2FrameReader` и превью.
6. Убрать дубли форматтеров (`Mp4Recorder::HrHex` → `vcam::HrHex`; `HostHotkey::HotkeyDisplay` → `vcam::FormatHotkey`).

### Этап 3. Настройки
7. Разделить `Settings` (C++ и C#) на data-struct + `SettingsLoader`/`SettingsSerializer`.
8. Вынести правила валидации (hotkey ranges, quality/capture tokens, scaleMode) в общие helpers; покрыть одинаковыми тестами.
9. Устранить расхождения (например, `\uXXXX` в C++ сейчас превращается в `?`).

### Этап 4. Разделение тяжёлых классов
10. `CMediaStream` → `V2FrameAcquirer` + `NegotiatedTypeResolver` + `SampleDelivery` + `StreamWorker`; `CMediaStream` как фасад COM.
11. `CameraSource` → разделить capture-поток, NV12-конверт, controls/control-server.
12. `FrameWriter` → v1-публишер + v2-публишер + общий pacing/cache.
13. Устранить дублирование `SettingsFileGuard` (один тип в `HostGlobals.h`, переименовать в `SettingsFileLock`).

### Этап 5. Глобальное состояние и диагностика
14. Свести глобалы `HostGlobals.h` в структуру контекста (`HostContext`), передаваемую в функции; оставить только процессные объекты.
15. `CAttrLogProxy` и `VCamDiagLog` — спрятать за `VCAM_DEBUG`/условную компиляцию или удалить.

### Этап 6. Тесты
16. Унифицировать тестовую сборку: линковать `ProducerCore.lib` в `src/Tests` или явно перечислять все нужные источники.
17. Расширить юниты на чистые helpers (NV12-конверт, score-сортировка, letterbox, settings-валидация, control-proxy parsing).
18. `VCamPreview` — перевести на общий читатель и общий letterbox; оставить только GDI+-рендер.

## 6. Риски

- Контракт shared memory (`SharedMemoryContract.h`) и `ProducerApi.h` frozen — изменения требуют согласования и обновления e2e-фаз.
- `MediaSource` не линкует `ProducerCore` — любое разделение должно сохранять эту границу или явно её пересматривать.
- Рефакторинг `CMediaStream`/`CameraSource` затрагивает критичный путь кадров; нужен прогон e2e после каждого этапа.
- .NET и C++ settings должны оставаться байт-совместимыми (golden-тесты).

## 7. Что не трогаем

- Схему `settings.json` (новая + миграция legacy) — только через согласование.
- Нумерацию версий установщика и ритуал FrameServer.
- Native-типы (ShouldAdvertiseNative=false) — страж e2e фаза H.
