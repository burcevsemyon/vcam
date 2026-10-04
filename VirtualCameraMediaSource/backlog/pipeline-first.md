# Pipeline-first: от «виртуальной камеры» к конвейеру кадров

Основание — интроспекция 04.10.2026: продукт описан как «виртуальная камера»,
хотя по коду ядро — realtime-конвейер кадров (source → engine → transform →
sinks), а камера — лишь один из sink'ов (shared-memory шина + MF-адаптер).
Цель: сделать пайплайн корнём архитектуры и кода, камеру — подключаемым
адаптером. Детали по тестам — `testing.md`, `regression-safety.md`.

Ограничения (решения пользователя 04.10.2026):

- **контракт v1 frozen** (`SharedMemoryContract.h:47` — «блок v1 выше — frozen,
  ни байтом»); всё новое — только ADD поверх v2+;
- камера обязана продолжать работать (Zoom / ktalk / «Камеры» Windows);
- установленные 0.0.x не ломаем без dual-name периода (имена секций, CLSID
  `{B2B674D4-9CF0-461C-BDCE-3D56FBB41356}`, задача `VCamHost`,
  `%APPDATA%\VCam\settings.json`, ярлыки).

## 0. Глоссарий (фиксируем язык)

| Термин | Что это в коде сегодня | Роль |
|---|---|---|
| **Source** | `IFrameSource` (`ProducerApi.h:38`), `StaticImageSource` / `VideoFileSource` / `CameraSource` | вход пайплайна, кадры BGRX |
| **Engine** | `Machine` + `Step` + `RenderOne`/`WriteOne` — **дублированы** в хосте (`VCamVideoStreamProducer.cpp:1109-1455`) и CLI (`VCamProducerCli.cpp:220-426`) | цикл 30 FPS, hot-switch, fallback |
| **Transform** | `PostProcessFrame` — no-op, живёт только в exe хоста (`VCamVideoStreamProducer.cpp:1286`) | стадия между source и sinks |
| **Sink** | `FrameWriter` (шина v1+v2), `Mp4Recorder` | выход пайплайна |
| **Bus** | `Global\VCam.FrameBuffer.v1/v2`, seqlock, 8/4 слота, ready-event | IPC-транспорт |
| **Camera adapter** | `MediaSource.dll` (IMFMediaSource2) + `Registrar.exe` + `SharedMemoryFrameSource` | потребитель шины = «виртуальная камера» |
| **Camera-in** | `CameraSource` (физическая веб-камера как источник) | source, НЕ адаптер |
| **Host** | `VCamVideoStreamProducer.exe` (tray), `VCamProducerCli.exe` | оболочка движка: UI, логи, жизненный цикл |

> «Камера» в проекте — два разных места: **camera-in** (физическое устройство
> как источник) и **camera-out** (виртуальное устройство как sink). Путаница —
> главный источник терминологического шума; в доках и логах различать явно.

## P0 — язык и границы (поведение не меняется)

- [ ] **P0.1. Глоссарий в доки.** Раздел «Архитектура: конвейер кадров» в
  `README.md` + правка таблицы каталогов в `AGENTS.md` (термины из п.0).
  Done: камера описана в подразделе «Адаптеры», ядро — как шина+sink'и;
  в описании ядра нет формулировки «продукт = камера».
- [ ] **P0.2. Инвентаризация camera-coupling.** Заполнить таблицу ниже:
  каждое место, где «камера» протекла в ядро, помечено
  `ядро` / `адаптер` / `историческое имя`. Done: таблица полная, спорные
  пункты вынесены в P3 (имена).
- [ ] **P0.3. Замок v1.** `static_assert` на layout уже есть
  (`SharedMemoryContract.h:68-70`) — добавить именной регресс-тест
  (layout + имена секций + `VCamFrameSize`) в будущий `VCamTests`
  (`regression-safety.md` P0.1). Done: правка v1-блока ломает сборку/тест.
- [ ] **P0.4. Инвариант «один писатель».** Зафиксировать в доке: два writer'а
  в одну секцию интерливят кадры (предупреждение CLI
  `VCamProducerCli.cpp:863-866` — сохранить). Fan-out sink'ов не должен
  плодить второго писателя v1. Done: правило описано, e2e-предусловие
  (остановить tray-хост) ссылается на него.

### Инвентаризация (стартовое заполнение P0.2)

| Где | Что | Класс |
|---|---|---|
| `SharedMemoryContract.h` | v1 frozen 720p (камерный формат), v2 native cap 4K | ядро/шина |
| `FrameWriter` | dual-write v1+v2, pacing 30 FPS, `FlushLast`, кэш `LastFrame720p()` | ядро (sink) |
| `MediaSource.dll`, `Registrar.exe`, `SharedMemoryFrameSource` | MF-адаптер, регистрация/холдер, heartbeat читателя | адаптер |
| `CameraSource`, `CameraDevices`, `CameraControls`, `ControlServer` | camera-in: захват, перечисление, IAM-контролы | ядро (source) |
| `Mp4Recorder` | запись эфира; читает кэш `FrameWriter` (`VCamVideoStreamProducer.cpp:1258-1278`) | ядро (sink), связь с другим sink'ом — дефект |
| `PostProcessFrame` | точка врезки, есть только в хосте (в CLI отсутствует) | ядро, расхождение хост/CLI |
| `Settings.quality` | свойство v2-секции, не источника | ядро (sink-конфиг) |
| `Settings.camera.*` | свойство camera-in | ядро (source-конфиг) |
| `VCamProducerCli status` | печатает «writer section v1 / v2 section» (`:139-218`) | историческое имя шины |
| Имена: `VCam*`, `vcam::`, `VCam.FrameBuffer.v1/v2`, `VCamVideoStreamProducer.Instance`, `VCamHost` | бренд = камера | P3 (оценка стоимости) |
| `e2e_test.ps1` | камеро-центричный (CaptureTest, inspect, device-фазы D/E, Registrar) | нужны pipeline-фазы (P2.4) |

## P1 — выделить pipeline-core (реальный рефакторинг)

- [ ] **P1.1. Экстракция движка.** `Machine`/`Phase`/`Step`/`RenderOne`/
  `WriteOne`/`EnsureFrameBuf`/`BeginSwitch`/`EnterFallback`/`FlushOrSleep`
  из двух exe — в общий `PipelineEngine` (в `ProducerCore` или новый
  `PipelineCore`). Лог-префикс `[host]`/`[cli]` — параметр: e2e матчит
  подстроки (`writer ready`, `[cli] switch:`, `[cli] active:`,
  `[cli] source opened: type=`, `[cli] no signal`, `[cli] open failed`).
  Done: дубль удалён, e2e зелёный **без правки ассертов**.
- [ ] **P1.2. `IFrameSink` + fan-out.** Интерфейс
  `Open/Write(bgrx, stride, w, h)/FlushLast/Close/Stats`; `FrameWriter` →
  `SharedMemorySink` (v1+v2 внутри, dual-write не меняется). Engine держит
  список sink'ов и ничего не знает про shared memory.
  Done: в файлах engine нет `#include "SharedMemoryContract.h"`.
- [ ] **P1.3. `Mp4Recorder` → `Mp4Sink`.** Убрать связь sink→sink: запись
  берёт кадр из движка, а не из кэша `FrameWriter::LastFrame720p()`
  (`FrameWriter.h:92-93`). Порядок source→transform→sinks сохранить —
  иначе mp4 разойдётся с эфиром. Done: запись работает при отключённом
  `SharedMemorySink` (харнес/тест).
- [ ] **P1.4. `PostProcessFrame` → стадия `ITransform` в engine.** Сейчас хук
  есть только в хосте (`:1286`), CLI пишет «сырьё» — расхождение.
  Done: оба хоста идут через одну цепочку; no-op остаётся no-op.
- [ ] **P1.5. `SourceFactory` → реестр.** `CreateSource` — if-цепочка
  (`SourceFactory.cpp:7-12`). Done: новый source = 1 файл + 1 регистрация.
- [ ] **P1.6. Конфиг по слоям.** Схема `settings.json` не меняется
  (совместимость), меняется трактовка в коде: `camera.*` → конфиг source'а,
  `quality` → конфиг bus-sink'а, `record.*` → конфиг mp4-sink'а,
  `hotkey`/`recordHotkey`/`autostart` → конфиг host'а.
  Done: `Settings` маппится в per-компонентные конфиги, `ToSourceConfig`
  не тащит sink-поля.

## P2 — камера как один из адаптеров

- [ ] **P2.1. Camera adapter как единица.** `MediaSource.dll` + `Registrar` +
  `SharedMemoryFrameSource` — описать и собирать как адаптер шины к MF;
  ядро от них не зависит. Done: пайплайн собирается и тестируется
  (CLI + fake sink) без адаптера и без elevated-регистрации.
- [ ] **P2.2. v2 как основной путь.** Инвентаризация: кто реально читает v2
  сегодня; план перевода camera-adapter на v2 (v1 остаётся legacy-зеркалом
  720p для совместимости). Done: решение записано, v1 не тронут.
- [ ] **P2.3. `status` → метрики пайплайна.** Per-sink seq / written / dropped /
  errors + состояние source и transform вместо «v1 section / v2 section»
  (`VCamProducerCli.cpp:139-218`). Done: вывод — таблица sink'ов, e2e-матчи
  `frames are being written` и `v2 section ... frames are being written`
  сохранены.
- [ ] **P2.4. E2E «пайплайн без камеры».** Fake source → in-memory/file sink →
  числовой ассерт кадров; камерные фазы (inspect, device D/E, Registrar)
  остаются отдельным SKIP-able блоком. Пересекается с
  `regression-safety.md` P1.3 (host loop на fake-источнике).
  Done: прогон зелёный на машине без веб-камеры.

## P3 — имена и каталоги (отдельное решение, высокий риск)

- [ ] **P3.1. Стоимость переименования `VCam*`.** Оценить: CLSID, имена секций
  v1/v2, мьютекс `VCamVideoStreamProducer.Instance`, stop-event, задача
  `VCamHost`, `%APPDATA%\VCam`, `%LOCALAPPDATA%\VCam\host.log`, установки
  пользователей, e2e-матчи, skills. Done: оценка + решение
  «делаем / не делаем» + dual-name план (старые имена работают переходный
  период).
- [ ] **P3.2. Каталоги под глоссарий.** `src/Common` + `src/ProducerCore` →
  `src/Pipeline` (bus / contract / engine / sources / sinks) и
  `src/Adapters/Camera` (MediaSource / Registrar). Done: структура = глоссарий;
  `.sln`, пути сборки, skills и `AGENTS.md` обновлены.

## Риски

- **e2e держится на подстроках логов** — любой рефакторинг строк ломает тест;
  список матчей держать в P1.1 и проверять после каждого шага.
- **Один писатель на секцию** — fan-out sink'ов не должен давать второго
  писателя v1 (интерливинг).
- **Деплой `MediaSource.dll`** — только ритуал FrameServer
  (`sc stop` → copy → `sc start`); рефакторинг ядра его не касается, адаптер
  без ритуала не обновлять.
- **Запись = «как в эфире»** — при переносе transform'а порядок стадий
  обязан сохраниться, иначе mp4 ≠ эфир (регрессия уже была: переворот строк).
- **v1 frozen** — любое «удобное» изменение контракта = регресс у потребителей.

## Не делать (осознанно)

- Не менять v1-контракт и имена секций в рамках этого перехода.
- Не добавлять новые sink'и (NDI / RTMP / websocket / pipe-вывод) — это
  продуктовое расширение, отдельный документ.
- Не переписывать `MediaSource.dll` / `MediaStream` «под не-камеру»: MF-адаптер
  остаётся как есть.
- Не переименовывать exe / CLSID / задачу до решения P3.1.
- Не писать движок «с нуля» — только экстракция существующего (иначе теряем
  проверенные pacing / `FlushLast` / fallback-тайминги).

## Порядок

P0 → P1.1 → P1.2 → P1.3 + P1.4 → P1.5 + P1.6 → P2 → P3 (по решению).
Каждый пункт независимо полезен; e2e зелёный после каждого шага.
