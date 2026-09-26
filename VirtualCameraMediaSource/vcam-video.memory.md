# vcam-video (parent memory)

## Задача
Video-провайдер + плавающее окно предпросмотра + UI. Разрешения от пользователя:
1. Масштаб видео — letterbox (рекомендация принята).
2. Окно — отдельное лёгкое приложение `VCamPreview` (always-on-top).
3. UI — в существующий `VCamSettingsUi`: переключатель static|video + выбор файла.
4. Одновременно static+video не нужны — ручной выбор провайдера.

## Ключевые факты (для subagent'ов)
- Репо: `C:\Users\Semen\source\repos\VCam\VirtualCameraMediaSource`, решение `VirtualCameraMediaSource.sln` (Release|x64).
- MSBuild: `"C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" VirtualCameraMediaSource.sln /p:Configuration=Release /p:Platform=x64 /m`; csproj — сначала `dotnet restore`.
- Контракт памяти: `src/Common/SharedMemoryContract.h` — `Global\VCam.FrameBuffer.v1`, `Global\VCam.FrameReady.v1`, `VCamSectionHeader`, 1280×720, stride 5120, RGB32(BGRX), `VCamFrameSize=3686400`, 8 слотов, seqlock, DACL `vcam::VCamDacSddl`.
- Референс-провайдер: `src/StaticProducer/StaticProducer.cpp` — каркас (CreateFileMapping+хедер, writer 30 FPS slot=(idx+1)%8 под CS, settings polling 500ms, Escape exit, stdout+fflush).
- Settings: `%APPDATA%\VCam\settings.json`; текущий формат `{imagePath, scaleMode, cropX/Y/W/H, cropKeepAspect}`. РАСШИРЯЕТСЯ: `mediaMode` (static|video), `mediaPath`. Старые поля не удалять (back-compat: mediaPath отсутствует → static берёт imagePath).
- Выбор провайдера — ручной (пользователь запускает нужный exe); mediaMode читается для логов/предупреждений, НЕ для автозавершения.
- Новые поля РЕАЛИЗОВАНЫ в обоих C++-провайдерах (чтение): `mediaMode` ("static"|"video"), `mediaPath` (строка, тот же JSON-escaping что и imagePath).
  - VideoProducer: источник = mediaPath ?? argv[1]; mediaMode!="video" → warning + продолжает; смена mediaPath → hot-reload (переоткрытие MF reader без перезапуска).
  - StaticProducer: eff = mediaPath если задан и mediaMode!="video", иначе imagePath (fallback — mediaPath); mediaMode=="video" → warning + продолжает; argv[1] как раньше переопределяет стартовый путь. crop/keepAspect не тронуты.
  - Писать новые поля может только UI (subagent 3); C++ обратной записи не делает.
- Тестовые ролики лежат в `e2e_output\`: `test_video.mp4` (5с, testsrc2 320x240), `test_video2.mp4` (такой же, для hot-reload), `pattern_top_white.mp4` (ориентация/letterbox).
- Тестовый E2E: `e2e_test.ps1`; CaptureTest direct mode: `CaptureTest.exe <n> <prefix>` → BMP 1280×720 24-бит (2 764 854 байт) в указанную папку.
- SaveBMP channel order уже исправлен (BGRX → BMP).
- Камера: регистрация elevated `Registrar.exe add VCam hold`; сейчас камера зарегистрирована, StaticProducer работает отдельно.
- Питфолы: GDI+ только HighQualityBicubic; не убивать пользовательские процессы без спроса (UI/продюсер юзера); лок — MSB3027 (закрыть exe).
- Питфол MF (VideoProducer): `SetCurrentMediaType(RGB32)` без атрибута ридера даёт `MF_E_INVALIDMEDIATYPE (0xC00D36B4)` — нужен `MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING=TRUE` (см. `vcam-video-producer.memory.md`).

## Статус
- [x] crop + cropKeepAspect + SaveBMP-фикс закоммичено `06b3b2b` — работает вживую.
- [x] VideoProducer (subagent 1) — done: MF SourceReader, frame-holding по ts, луп SetPosition(0), letterbox-билиней, hot-reload mediaPath; приёмка 1-7 зелёная; риски: RGB32 требует ENABLE_VIDEO_PROCESSING (fallback-цепочка), negative stride не проверен.
- [x] VCamPreview (subagent 2) — done: Win32 always-on-top, seqlock-read, StretchDibits top-down, NO SIGNAL + автопереподключение; приёмка зелёная (снимки: nonBlack=117k, topmost=0x108).
- [x] UI static|video (subagent 3) — done: mediaMode/mediaPath, выбор видео с фильтром, video-панель с кнопкой запуска VCamPreview, back-compat imagePath; round-trip 19/19, UIA-смоук 21/21, e2e exit=0; risk: test-seam `filePath` в Settings.Load/Save.
- [ ] Ручная проверка пользователем (видео в UI → VideoProducer → VCamPreview поверх окон).
- [ ] Коммит (спросить).

## Память субагентов
- Промпт субагента += этот файл (память вышестоящей) + свой `<task>.memory.md` (писать статус/факты/риски).
