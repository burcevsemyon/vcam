# vcam-camera (parent memory) — источник «физическая камера»

## Задача
Третий тип источника кадров `L"camera"`: захват с веб-камеры через Media Foundation →
трансляция в `Global\VCam.FrameBuffer.v1` через существующий пайплайн хоста
`VCamVideoStreamProducer` (трей, автозапуск, hot-switch, fallback).

Ветка: `feature/vcam-camera-source` (от `main`, `9ea061c`). Память вышестоящей:
`vcam-producer.memory.md` (архитектура хоста, решения, питфолы, команды сборки)
+ `vcam-producer-core.memory.md` (API ProducerCore) + `vcam-producer-cli.memory.md`
+ `vcam-producer-tray.memory.md`.

## Утверждённые пользователем решения (26.09.2026)
1. **Нет камеры/занята/отключена → NO SIGNAL** (штатный fallback хоста), без автоподбора
   другого устройства. Recovery — штатные ретраи фазы Fallback (1 с).
2. **Выбор устройства — UI-список** (enumerate) + запись id/имя в секцию `camera`;
   вручную JSON править можно.
3. **Подгонка кадра — честный fit (letterbox)**, без искажений; ровно 1280x720 → memcpy.
4. **CLI**: `list-devices` (перечисление) + `run --type camera [--device X]`.
5. **fps фиксированный 30** — поля fps в секции camera НЕТ.
6. **Пустая секция camera (id и name пустые) → NO SIGNAL до выбора** (не «первая камера»).
7. **e2e — фаза C** с камерой: нет устройства → `PASS SKIP`, e2e остаётся exit 0.

## Схема settings (расширение новой схемы)
```json
{ "source": {"type": "static|video|camera"},
  "static": {...}, "video": {"path": "..."},
  "camera": {"id": "<MF symbolic link>", "name": "<friendly name>"},
  "autostart": bool }
```
- `id` = `MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID` (symlink, стабилен для USB-порта),
  `name` = `MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME`.
- Матчинг в CameraSource: точный id (i-cmp) → точное имя → подстрока имени → иначе Open=false.
- FindKeyPos уже требует `:` после ключа — `"type": "camera"` не совпадёт с ключом `camera`.

## Контракт
- `IFrameSource` (Open/Render/Close/Name), `CreateSource(type)` в `SourceFactory.cpp`.
- `SourceConfig`: НОВОЕ поле `camName` (+ `path` = `id` — работают существующие логи
  `path=%s` в хосте/CLI), `operator==` расширен.
- Статус/лог в хосте/CLI для camera: показывать `camName` если не пусто, иначе `path`
  (иначе в трее длинный symlink).
- `SharedMemoryContract.h` НЕ менять; старые exe не трогать; UTF-8 без BOM;
  `/utf-8` уже в `Directory.Build.targets`; чужие процессы (Registrar/Preview/UI) не убивать.

## План (последовательно, лимит 2 раунда на субагента)
- [x] **Subagent A — ProducerCore** (DONE, 1 раунд, см. `vcam-camera-core.memory.md`):
      `CameraDevices.h/.cpp` (EnumerateCameraDevices + OpenCameraActivate), `CameraSource.h/.cpp`
      (фоновый захват, кэш под mutex, silence-counter 90, letterbox fit), секция `camera`
      в Settings, SourceFactory `L"camera"`, `SourceConfig.camName`. Сборка exit 0,
      smoke 35/0 (enumerate count=1 «Brio 90»).
- [x] **Subagent B — CLI + e2e** (DONE, 1 раунд, см. `vcam-camera-cli.memory.md`):
      `list-devices` (stdout `id\tname` UTF-8, шапка в stderr), `--type camera --device <id>`,
      метка TargetLabel (camName→path), LooksNewSchema += camera; e2e **фаза C**:
      list-devices → run camera → opened/active/seq → негатив (bad id → no signal, 19 ретраев)
      → SHA256-restore. e2e exit 0, 0 failures.
- [x] **Subagent C — UI** (DONE, 1 раунд, см. `vcam-camera-ui.memory.md`):
      `SourceType.Camera` + 3-й пункт `_mediaCombo` + `_cameraPanel` (список через
      `VCamProducerCli list-devices`, FindCliExe как VCamPreview), Settings.cs Load/Save
      camera; тесты **43/43** (25 старых + 18 camera); смоук UI (UIA): Brio 90 выбрана,
      Save пишет camera-секцию, settings восстановлен SHA256-совпадение.
- [x] **Основной**: правка хоста `VCamVideoStreamProducer.cpp` (TargetLabel: статус/логи
      показывают camName вместо symlink) + README (состав/CLI/e2e/таблица полей/новая
      секция «Физическая камера (camera)»/файлы). Сборка решения exit 0.

## Живая проверка (основной, 26.09.2026) — все пункты ТЗ закрыты
- e2e (skill vcam-e2e): **SUCCESS, 0 failures, exit 0**, фаза C PASS + негатив PASS.
- `run` с секцией camera в settings (без overrides): `source opened: type=camera path=Brio 90`
  + `active`; **Preview mean = 76.5** (NO SIGNAL ≈ 15–19 → поток виден);
  CaptureTest виртуалки: 3 BMP nonBlack=1722/1722/1720 → пайплайн end-to-end жив.
- hot-switch static↔camera (честный путь, правка settings): 3 × `[cli] switch:`,
  camera и static открыты, **все 10 BMP в окно переключения не чёрные** (нет чёрного экрана).
- NO SIGNAL: (а) несуществующий `--device` → no signal, не active; (б) пустая секция
  camera → `open failed: камера не выбрана` → no signal, не active.
- settings.json восстановлен байт-в-байт (SHA256 317B6114…1F61FA), процессов VCam не осталось.

## Факты/питфолы (для следующих сессий)
- MF-реальность (эмпирика subagent A): symlink читается из `VIDCAP_SYMBOLIC_LINK` ({58F0AAD8}),
  НЕ из значения VIDCAP-типа; `MFCreateSourceReaderFromURL(symlink)` → 0x80070002 — рабочий
  путь `ActivateObject` + `MFCreateSourceReaderFromMediaSource`; строковые атрибуты — VT_LPWSTR.
- Камер в системе сейчас **1** (Brio 90; ранее CaptureTest видел 2 — вторая недоступна, факт).
- CLI пишет логи UTF-8 в stdout-файл → PowerShell `Get-Content` без `-Encoding UTF8` даёт
  кракозябры; читать `[IO.File]::ReadAllText(..., UTF8)`.
- `run --type camera` БЕЗ секции camera в settings и без `--device` → NO SIGNAL — это
  утверждённое поведение («пустая секция → до выбора в UI»), а не баг.
- PS5.1: `.ps1` с кириллицей без BOM ломает парсер → писать проверочные скрипты чисто ASCII.
- Риск (из A): зависший навсегда ReadSample → Close join 3 с не освобождает reader (утечка);
  счётчик молчания ловит только возвраты из ReadSample.

## Git
- Ветка `feature/vcam-camera-source`, коммитов НЕТ (ждём явного разрешения пользователя).

## Питфолы (из памяти вышестоящей)
- MSB3027 — закрыть свои exe перед сборкой; csproj — сначала `dotnet restore`.
- MSBuild: `"C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe"
  VirtualCameraMediaSource.sln /p:Configuration=Release /p:Platform=x64 /m /v:m /nologo`
- PS5.1: `[IO.File]::WriteAllText(..., UTF8Encoding($false))` — BOM-ловушка.
- Камера ранее работала через CaptureTest (`CaptureTest.exe inspect` count=2);
  если занята/отсутствует — зафиксировать факты, не подгонять проверку.

## Статусы субагентов
- (заполняются по мере выполнения)
