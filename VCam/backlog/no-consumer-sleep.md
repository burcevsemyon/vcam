# Sleep источников при отсутствии потребителей

Статус: план, ветка `feat/no-consumer-sleep`.

## Идея

Когда ни одно приложение не смотрит виртуальную камеру, источники медиа
(camera/video/static) переходят в паузу: нет захвата/декодирования/рендера →
CPU/GPU ≈ 0. Память (буферы, handles, открытая секция) не высвобождается.
При появлении MF-потребителя — быстрый wake.

## Решения (согласовано)

| Вопрос | Решение |
|---|---|
| Что засыпает | Пауза источников (память не трогаем), не освобождение |
| Кто потребитель | Только MF-клиенты (heartbeat `readerLastActiveTick`) |
| Порог сна | 60 с отсутствия потребителя |
| Поведение сна | Пробуждение по heartbeat (~1–2 с), не NO SIGNAL |
| Наблюдаемость | `status --json` + `host.log` |

## Ключевые факты из research

- Обратный канал **уже есть**: MediaSource пишет `readerLastActiveTick` в хедер
  секции при `StartForSession` (MediaStream.cpp:438) и при каждой доставке кадра
  (SharedMemoryFrameSource.cpp:166-168). Хост читает через `IsConsumerActive()`
  (HostCameraLifecycle.cpp:31-55, порог 3 с).
- Idle-механизма в хосте нет: `WorkerProc` ждёт INFINITE на stop/dirty и пишет
  всегда (VCamVideoStreamProducer.cpp:59-131), `PipelineEngine::Step`
  (PipelineEngine.cpp:84-182).
- **Registrar hold-watch**: seq не двигается И heartbeat протух 30 с → процесс
  выходит, камера исчезает из списка устройств (Registrar/main.cpp:92-171).
  Сон хоста обязан учитывать это.
- Fallback 7 с → NO SIGNAL живёт на стороне читателя
  (SharedMemoryFrameSource.cpp:17,256-266).
- Контракт v1 frozen (SharedMemoryContract.h:22-40), зарезервирован только
  `reserved`; v2-секция read-only для читателя, heartbeat туда не пишется.
- Потребители, не видимые heartbeat: VCamPreview, CLI, CaptureTest.

## Риски

1. **FrameServer может держать `RequestSample` живым** даже без клиентов →
   heartbeat не протухает, сон не наступит. Вне репо → эмпирика.
2. hold-watch (30 с) при сне > 30 с → камера пропадает; решается keepalive-записью.
3. Гонка «сон ↔ подключение»: порог 60 с ≫ 7 с fallback; wake дешёвый,
   т.к. источник не закрывается.
4. Native-probe v2 при протухшей секции после сна — проверить лесенку 3 типов
   после wake (ловушка ShouldAdvertiseNative).
5. E2E матчит `writer ready` / движение кадров — сон не должен срабатывать
   в тестовых паузах (60 с > пауз, но подтвердить; для теста — `power.idleSec`).
6. Многопотребительность: `readerLastActiveTick` одно на все MF-сессии — для
   сна это ок (любой активный клиент будит).

## Фазы

### Фаза 0 — spike (гейт фичи)
Проверить на живой системе: после закрытия всех приложений протухает ли
`readerLastActiveTick` (логировать тик, `status --diag`, msrc_diag.log,
DiagEvent `StartForSession`/`StopForSession`). Если FrameServer продолжает
pull — сон невозможен на этом сигнале, нужен альтернативный wake/trigger
(например, реакция на `StopForSession`).

### Фаза 1 — FSM в хосте
- Таймаут ~1 с в `WaitForMultipleObjects` вместо INFINITE.
- Состояния: Awake → IdleCountdown (60 с) → Paused → Wake.
- Sleep: `IFrameSource::SetPaused(true)` + FrameWriter прекращает pacing.
- Wake: свежий heartbeat / `dirty` / `stop`.
- Keepalive в сне: раз в ~20 с `FrameWriter::FlushLast` (memcpy кэша,
  рендера нет) — держит `seq` для hold-watch и v2-заголовок для probe.

### Фаза 2 — пауза источников (ProducerApi)
- `IFrameSource::SetPaused(bool)`:
  - CameraSource: capture-thread ждёт event вместо `ReadSample`;
  - VideoFileSource: цикл ждёт вместо декодирования;
  - StaticImageSource: пропуск рендера.
- Open/буферы/handles не закрываем.

### Фаза 3 — наблюдаемость
- `HostStatus`: `sleep.state|since|wakes|lastWakeMs` в `status --json`.
- `host.log`: sleep/wake строки.
- Settings: `{"power":{"sleep":true,"idleSec":60}}` (hot-reload).

### Фаза 4 — тесты
- Doctest: FSM на фейках (heartbeat + source), keepalive < 30 с.
- E2E: фаза с `idleSec=5` — сон наступает, wake по подключению, кадры идут;
  убедиться, что A/B/G не ловят сон.
- Native-лесенка после wake = 3 типа.
- Питфоллы — в AGENTS.md.

## Приёмка

1. 60 с без MF-клиентов → CPU/GPU источников ≈ 0, камера в списке, память цела.
2. Подключение потребителя → кадры ≤ ~2 с, без видимого NO SIGNAL.
3. `status --json` и логи отражают sleep/wake.
4. e2e + юниты зелёные.
