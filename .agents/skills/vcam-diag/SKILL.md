---
name: vcam-diag
description: >-
  Диагностика VCam средствами наблюдаемости (P0–P2.3): порядок status --json →
  логи → счётчики → VCAM_DEBUG → пакет/дамп; симптом → какие сигналы смотреть
  (writer/seq/failopen/runtime/REC/transients/runstate/msrc_diag); stale-секции,
  ротация .old, границы приватности, когда нужен дамп или diag-пакет. Дополняет
  таблицы vcam-e2e (там — e2e/ktalk-эмпирика; здесь — инструменты). Use when:
  «нет сигнала»/статика/вытянута/запись не идёт/хоткей молчит/после ребута нет
  сигнала/контрол недоступен, разбор host.log/msrc_diag, чтение счётчиков,
  сбор diag-пакета для issue, решение дамп-или-логи.
---

# Skill: vcam-diag — диагностика средствами наблюдаемости

## Порядок действий (всегда в этом порядке)

1. **`status --json` одним вызовом** — состояние + деградации числами:
   `VCamProducerCli.exe status --json | ConvertFrom-Json`.
   Смотри: `hostRunning`, `v1/v2.open+growing+seq`, `rec.active`, `hotkey.borrowed`,
   `failopen.*`, `runtime.*` (`null` = источник недоступен, не ошибка).
   Это отвечает на 80% вопросов без открытия логов.
2. **Логи** — только если JSON не дал ответа: хвост `host.log`
   (`%LOCALAPPDATA%\VCam\`), лог CLI-продьюсера (stdout редирект), окно
   `msrc_diag.log` (см. §msrc ниже). Формат единый:
   `[дата время] [уровень] [host|cli|msrc] сообщение`; матчь подстроки.
3. **Счётчики/транзенты** — детализация деградации: `%APPDATA%\VCam\`
   `failopen_counters.json`, `runtime_counters.json`, `record_state.json`,
   `hotkey_state.json`, `host_runstate.json`.
4. **`VCAM_DEBUG=1`** — только если info не хватило: рестарт процесса с флагом,
   `[debug]`-строки (полный SourceConfig, резолв путей). Для msrc в svchost —
   системная переменная + рестарт FrameServer.
5. **Пакет/дамп** — для issue: `VCamProducerCli.exe diag` (каталог-артефакт);
   при падении — `%LOCALAPPDATA%\VCam\Crashes\*.dmp` (список в пакете, сами дампы
   — по запросу).

## Симптом → сигналы (карта)

- **«Нет сигнала» (превью/потребитель).**
  `--json`: `v1.open`? `growing`? Если `open=false` — писатель не запущен
  (проверь мьютекс/процесс хоста). Если `open=true growing=false` — писатель встал:
  `failopen.sourceOpen` (источник не открывается?) + host.log хвост
  (`open failed` / `no signal` / `config applied` — с каким конфигом умирал).
  Если продьюсера нет вообще, а секция открыта — stale (см. ниже).
- **«Статичная картинка» (seq заморожен).**
  `--json` `growing=false` при живом продьюсере = источник встал (камера/файл),
  НЕ media source. `status` → «no new frames». Причина — в логе продьюсера
  (host.log или CLI stdout): `ReadSample failed`, `open failed`, конец файла.
  Потребитель при этом отдаёт кэш (FallbackFrame) — «статика», не «нет сигнала».
  Если продьюсер мёртв >7 с — сменится на NO SIGNAL (lifecycle).
- **Stale-секция (секция есть, писателя нет).**
  `--json`: `open=true growing=false` + `hostRunning=false` + процесса нет =
  труп прошлого рана (Kill/крах). Лечится рестартом продьюсера (пересоздаст).
  `status --ready` такое отсекает (требует роста seq), полный `status` — показывает.
- **«Вытянута / только верх / letterbox».**
  `--json` v2 dims + `quality` + msrc `deliver type` / `negotiated … allocator type …`
  + `Lock … maxLen=` (1228800=640×480 RGB32, 3686400=720p RGB32, 1384448=NV12 720p).
  Рассогласование размера доставки vs буфера аллокатора. Детали — vcam-e2e таблицы.
- **«Запись не идёт / не стартовала».**
  `--json` `rec.active`? + `failopen.recordStart` + host.log (`record start failed` /
  `record started` / `record write failed`). `runtime.recFrames/recDropped` —
  пишет ли энкодер. Путь — `record_state.json` (активна) или `config applied`
  (настроен). Команда старта через `record_command.json` — только при живом хосте
  (prestart съедает CleanupStale как stale).
- **«Хоткей не работает».**
  `failopen.hotkey` (занят?) + host.log (`RegisterHotKey … failed` + код; 1409 =
  уже занят другим приложением) + `hotkey.borrowed` (одолжен под video?).
- **«После ребута нет сигнала».**
  host.log начало: `previous run did not shut down cleanly (pid=.. last step: ..)`?
  → прошлый ран упал (смотри lastStep + хвост + дамп). Нет маркера → смотри
  `token: elevated=N SeCreateGlobalPrivilege=N` (0 = Limited-токен из Run,
  норма = 1/2 от задачи VCamHost) + `writer ready (Local\…)` (= нет привилегии,
  раскол секций) + задача `VCamHost` (`autostart_task.txt` в пакете).
- **«Контрол недоступен».**
  `failopen.control` + `get-control` вывод (`not supported by this device`).
  `E_PROP_ID_UNSUPPORTED` — нормализованный ответ прокси, не ошибка транспорта.
- **«Битый конфиг / странное поведение после правки».**
  host.log/CLI: `settings: broken/unrecognized JSON - using defaults` + следующая
  `config applied` покажет применённый дефолт. Валидируй JSON до записи.

## msrc_diag.log — как читать

- Пути: `%ProgramData%\VCam\msrc_diag.log` (общий) + `%TEMP%\VCam\msrc_diag.log`
  процесса (юзер → свой TEMP, svchost → свой temp). Формат P0.1 с датой.
- Читай ОКНОМ (offset до/после события), не весь файл; фильтр по `pid=` актуального
  svchost; после ротации смотри и `.old` (cap 10МБ). Device-режим грузит
  installed-копию (`C:\Program Files\VCam\`) — сверяй SHA256 build vs installed,
  иначе диагностируешь старую DLL.
- Маркеры прокси-пути: `negotiated … -> allocator type …`, `Lock hr=0x… maxLen=…`,
  `buffer too small` (= баг размера, не молчи). `deliver type` — только на смену типа.

## Границы (что НЕ делаем)

- В логи не попадают кадры, содержимое видео, PII; пути пользователя — только
  `[debug]` и `settings.json` в пакете (предупреди отправителя проверить перед отсылкой).
- Дамп — только при падении (Crashes), не для «странного поведения» (там логи/счётчики).
  MediaSource in-proc не дампим (чужой svchost) — там msrc-хвост + runstate хоста.
- ETW — только если всего выше не хватило (пока не понадобилось ни разу, P2.4 skip).
- `diag`-пакет — чтение only; собирается локально, отсылка — решением пользователя.
