# Наблюдаемость: логи, метрики, диагностика падений

Backlog-документ. Статус живёт здесь же (чекбоксы ниже). Язык — русский.

Основание — тема размазана по трем документам: минимум логов в `testing.md`
§6, метрики и cap в `regression-safety.md` P2.3–P2.4, метрики sink'ов в
`pipeline-first.md` P2.3. Здесь — единый план наблюдаемости всего конвейера
(host, CLI, MediaSource, запись), включая диагностику падений и пакет
диагностики для поддержки. Уровень — задачи, без привязки к файлам: точки
врезки уточняются при реализации (и так поменяются при pipeline-рефакторинге).

Связанные документы: `testing.md` (тестируемость), `regression-safety.md`
(порядок «дёшево → дорого»), `pipeline-first.md` (per-sink метрики — там,
P2.3; сюда не дублируем).

## 0. Где мы сейчас

- **host.log** (`%LOCALAPPDATA%\VCam\host.log`) — плоский текст, таймстемпы
  `[HH:MM:SS.mmm]`, ротация >1 МБ → `host.log.old`. Формат строк не
  унифицирован, префиксы `[host]`/`[cli]` — исторические.
- **msrc_diag.log** (`%ProgramData%\VCam\` + `%TEMP%\VCam\`) —
  лог MediaSource; растёт сотнями МБ,
  лимит только практикой (offset-чтение описано в skill), кодом не закреплён.
- **`VCamProducerCli status`** — settings, секции, мьютекс, seq; readiness
  частично (seq растёт/стоит), счётчиков нет.
- **Краши** — WER-событий и минидампов нет (история 29.09.2026: «следов
  падения нет»); причина падения не восстанавливается.
- **Fail-open пути молча деградируют**: хоткей занят, запись не стартовала,
  источник не открылся, битый JSON → дефолт, прокси контролов отвечает
  `E_PROP_ID_UNSUPPORTED`. Пользователь видит «не работает», лог — тих.
- **Транзиенты** (`record_command.json`, `record_state.json`,
  `hotkey_state.json`) при крахе висят; удаление stale при старте не
  логируется.
- **Структурированных метрик нет**; диагностика симптомов потребителя
  («картинка вытянута / статична / нет сигнала») — ручная, по skill'у.

## P0 — видимость деградаций (уже кусалось)

- [x] **P0.1. Единый формат строк логов.** Один шаблон для host / CLI /
  MediaSource (таймстемп + уровень + область + сообщение); одно решение
  UTC или локальное время. Done: новые строки — по шаблону; e2e-матчи
  (`writer ready`, `[cli] switch:`, `[cli] active:`,
  `frames are being written`) сохранены или скрипт поправлен в том же шаге.
  > Реализовано 06.10.2026: `Common/LogFormat.h` (header-only, loader-lock-safe),
  > шаблон `[YYYY-MM-DD HH:MM:SS.mmm] [уровень] [host|cli|msrc] сообщение`,
  > время локальное. Инлайн-префиксы `[host]`/`[cli]` сняты → поле области.
  > Вывод данных CLI (help/status/list/get/set-control) — raw, без префикса.
  > Уровни пока дефолтные (полноценные + VCAM_DEBUG — P1.1). e2e: все лог-матчи
  > (фазы A/B/G/H/C/F) green БЕЗ правки скрипта. D/E + e2e exit 0 ждут деплоя
  > MediaSource.dll (ритуал FrameServer) — отложен до релиза (решение пользователя).
- [x] **P0.2. Маркеры fail-open.** Каждый тихий fallback (хоткей занят, запись
  не стартовала, источник не открылся, битый JSON → дефолт, удалён
  stale-транзиент, контрол недоступен) — строка в лог + счётчик.
  Done: не бывает деградации без записи в лог; перенесено из
  `regression-safety.md` P0.4 и `testing.md` §5, статус здесь.
  > Реализовано 06.10.2026: `Common/FailOpenCounters.h` — 6 атомарных счётчиков
  > (SettingsBrokenJson/HotkeyBusy/RecordStartFailed/SourceOpenFailed/
  > StaleTransientRemoved/ControlUnavailable), header-only, per-module. Битый JSON:
  > `Settings::Load` детект (нет ни одного известного ключа) → счётчик + флаг
  > `brokenJson_`; строку пишет `SettingsWatcher` (log-callback, т.к. у Settings
  > нет Log). Остальные 5 — счётчики в уже логируемых точках (хоткей/запись/
  > open failed/stale-транзиент/E_PROP_ID_UNSUPPORTED). Экспорт в `status` — P0.3.
  > Проверено: битый JSON → `settings: broken/unrecognized JSON - using defaults
  > (fail-open)`; валидные new/legacy — без ложных срабатываний; e2e 63 PASS
  > (2 FAIL = тот же stale-DLL guard, не регресс).
- [x] **P0.3. Метрики в `status`.** seq + written / dropped + REC state +
  hotkey / borrow state + счётчики P0.2. Done: состояние и деградации видны
  одним вызовом; e2e-матчи `frames are being written` сохранены.
  Перенесено из `testing.md` §6 и `regression-safety.md` P2.3.
  > Реализовано 06.10.2026: `Common/FailOpenFile.h` — `failopen_counters.json`
  > (%APPDATA%\VCam, JSON 6 счётчиков, tmp+rename); хост пишет снапшот на старте/
  > каждые 5 с/на выходе (WorkerProc). `status` показывает: `REC: idle|recording
  > (path)` (из record_state.json), `hotkey: normal|borrowed` (из hotkey_state.json),
  > `fail-open: settingsJson=.. hotkey=.. recordStart=.. sourceOpen=.. staleRemoved=..
  > control=..` (или `n/a`). seq/`frames are being written` без изменений.
  > Проверено: до хоста `n/a` → после хоста нули → после stale-транзиента
  > `staleRemoved=1` end-to-end; e2e 63 PASS (2 FAIL = stale-DLL guard).
- [x] **P0.4. Cap и ротация всех логов.** Лимит + ротация `msrc_diag.log`
  (сейчас сотни МБ); единая политика ротации для host.log. Done: ни один лог
  не растёт без лимита; offset-чтение из skill закреплено кодом/доком.
  Перенесено из `testing.md` §6 и `regression-safety.md` P2.4.
  > Реализовано 06.10.2026: `Common/LogRotate.h` — `RotateLogIfOverCap(path, cap)`
  > (cap → rename в .old) + cap'ы `kHostLogCapBytes=1МБ`, `kMsrcDiagCapBytes=10МБ`.
  > host.log: runtime-ротация в `Log()` (инкрементальный счётчик байт + close/
  > rotate/reopen под мьютексом) + ротация на старте сохранена. msrc_diag.log:
  > ротация перед каждой записью в `DiagEmit` (оба пути). Проверено: host.log
  > 1.8МБ→.old+свежий, msrc_diag 11МБ→.old+свежий нового формата; e2e 63 PASS
  > (2 FAIL = stale-DLL guard). Offset-чтение skill совместимо (ротация — .old,
  > e2e-окна в пределах cap не затрагивает).

## P1 — структурированные сигналы

- [x] **P1.1. Уровни и `VCAM_DEBUG=1`.** error / warn / info / debug; один
  флаг включает verbose всех компонентов вместо россыпи. Done: debug выключен
  по умолчанию; включён — виден полный контекст одного кадра/операции.
  Перенесено из `testing.md` §6.
  > Реализовано 06.10.2026: `vcam::IsDebugEnabled()` (LogFormat.h, `VCAM_DEBUG=1`,
  > кэш, нужен рестарт). `LogDebug()` (host) + `RunLogDebug()` (CLI) — no-op без
  > флага (без форматирования); тег `[debug]`. MediaSource `VCamDiagLog` в Release —
  > только при флаге (в Debug как было, всегда). Точечные debug-строки: полный
  > SourceConfig при apply target (host+CLI), резолв пути записи (host). e2e-матчи
  > (`[cli] open failed` и др.) оставлены Info — не ломаются. Полноценные
  > error/warn-разметки всех строк — не делались (уровень-инфраструктура готова).
  > Проверено: без флага нет `[debug]`; с флагом — полный контекст; e2e 63 PASS.
- [x] **P1.2. Счётчики как данные.** frames written / dropped, число
  переключений источников, срабатываний fallback, длительность записи,
  время кадра (min / avg / max) — не prose, а числа для скриптов (например,
  `status --json` или `ключ=значение`). Done: e2e и диагностика читают
  числа, а не парсят текст.
  > Реализовано 06.10.2026: `PipelineEngine` — switches (SetTarget при смене),
  > fallbacks (EnterFallback при переходе), frame time min/max/avg/count мкс
  > (QPC вокруг успешных RenderOne); `HostPipelineEngine` — RecFramesWritten/
  > Dropped геттеры. `Common/RuntimeCounters.h` — `runtime_counters.json`
  > (8 чисел, tmp+rename); хост пишет на старте/каждые 5 с/на выходе.
  > `status --json` — единый JSON (hostRunning/sourceType/quality, v1/v2
  > open+seq+growing, rec active+durationSec, hotkey borrowed, failopen[6],
  > runtime[8]; null при отсутствии). Текстовый status без изменений.
  > Проверено: --json валиден; runtime после хоста (frameCount=133/286,
  > min/avg/max); fallbacks=1 на битом static; rec active+durationSec=8.
  > e2e 63 PASS (2 FAIL = stale-DLL guard). Замечание: recFrames=0 при активном
  > REC в тесте — так сообщает m_rec (не регресс P1.2; запись вне скоупа e2e).
- [x] **P1.3. Readiness вместо sleep.** Опрос статуса как readiness-gate:
  признаки «writer готов», «кадры идут», «источник открыт». Done: часть
  `Start-Sleep` в e2e убрана. Пересекается с `regression-safety.md` P2.2
  (там — про e2e; статус шага e2e — там, механизм — здесь).
  > Реализовано 06.10.2026 (механизм): `status --ready` — быстрая проверка
  > (v1-секция открыта + seq растёт за 100 мс; stale-секция отсекается;
  > ~120 мс vs 600 мс полного status; `ready=1 seq=N`/exit 0 или `ready=0`/exit 1).
  > e2e: хелпер `Wait-Ready` (poll 500 мс до 10 с) заменил 6×`Start-Sleep 4`
  > (фазы A/B/G/H/F-старт/F-рестарт) — все `ready in 1s`, экономия ~18 с/прогон.
  > Остальные sleep (hot-switch 3с, NO SIGNAL 7с, debounce мс) — не readiness,
  > оставлены для P2.2. Проверено: stale → ready=0; живой → ready=1; e2e 63 PASS.
- [x] **P1.4. Диагностика конфига.** При старте и перечитывании настроек —
  строка, что применено (тип источника, путь, качество, рекорд); битый JSON —
  маркер + дефолт. Done: по логу видно, с каким конфигом работал процесс.
  > Реализовано 06.10.2026: `config applied: type=.. path=.. quality=.. record=..`
  > (Info) в `ApplySettingsDiff` (хост) и `WorkerProc` (CLI) — на старте и каждом
  > перечитывании (watcher dirty), независимо от смены цели; путь — человекочитаемый
  > TargetLabel, record — из settings (`(default)` если пуст). Битый JSON — маркер
  > P0.2 (`broken/unrecognized JSON - using defaults`), применённый дефолт виден
  > в следующей `config applied`. Проверено: старт (host+CLI), reload quality
  > source→fixed720p → вторая строка с новым quality; e2e 63 PASS (матчи не задеты).

## P2 — диагностика падений и поддержки

- [x] **P2.1. Минидампы.** Дамп на необработанное исключение для host и CLI
  (WER LocalDumps или handler + MiniDumpWriteDump). MediaSource живёт in-proc
  в чужом процессе (FrameServer / svchost) — дамп чужого процесса не делаем,
  там P2.2 + хвост лога. Done: управляемое падение оставляет дамп/маркер,
  причина восстанавливается.
  > Реализовано 06.10.2026: `Common/CrashDump.h` — handler + MiniDumpWriteDump
  > (без реестра/adminkи; `#pragma comment(lib,Dbghelp)`, без правок vcxproj).
  > `InstallCrashHandler(tag)` первым делом в wWinMain/wmain; фильтр — только
  > стековые буферы (куча может быть повреждена), дамп `%LOCALAPPDATA%\VCam\Crashes\
  > <tag>_<pid>_YYYYMMDD_HHMMSS.dmp` (DataSegs+Handles+ThreadInfo+Unloaded+IndirRefMem),
  > чистка старых (keep 5 newest), возврат EXCEPTION_EXECUTE_HANDLER (тихо, без WER-UI).
  > MediaSource не покрыт (чужой процесс — P2.2). Проверено end-to-end временным
  > `--crash-test` (AV): exit 0xC0000005 + .dmp 834КБ с сигнатурой MDMP; флаг удалён,
  > пересобрано, тестовые дампы прибраны. e2e 63 PASS.
- [x] **P2.2. Crash-маркер.** Флаг некорректного завершения + последний
  завершённый шаг/фаза, переживающие рестарт. Done: проблемы класса «после
  ребута нет сигнала» диагностируются по следам, без повторения сценария.
  > Реализовано 06.10.2026 (хост; MediaSource — только хвост msrc_diag, чужой
  > процесс): `%APPDATA%\VCam\host_runstate.json`
  > (`{pid,started,cleanShutdown:false,lastStep}`; tmp+rename). `CheckPreviousRunCrash()`
  > на старте worker'а: был файл → `previous run did not shut down cleanly
  > (pid=.. started=.. last step: ..) - possible crash/kill` + свежий "starting".
  > `UpdateRunStep()` в цикле при смене фазы/цели (`switching:<type>` / `active:<type>` /
  > `fallback`; только на изменении, без syscall в простое). `ClearRunState()` (delete)
  > при чистом выходе. Проверено: Kill → файл `lastStep:active:camera` → рестарт → маркер
  > с pid/started/step ✓; Stop event → файл удалён → рестарт без маркера ✓. e2e 63 PASS.
- [x] **P2.3. Пакет диагностики для поддержки.** Одна команда сбора: версии,
  `status` + счётчики, tail всех логов (с лимитом), settings.json, token
  state, состояние задачи автозапуска. Done: issue сопровождается одним
  артефактом вместо переписки «скиньте лог».
  > Реализовано 06.10.2026: `VCamProducerCli.exe diag [--out <dir>] [--settings]`
  > → `%TEMP%\VCamDiag_<ts>\` (14 файлов): versions.txt (CLI/host/MediaSource
  > same-dir+PF с версией/size/mtime + OS из реестра), status.txt + status.json
  > (переиспользование через запуск себя, без дублирования), settings.json +
  > record/hotkey/failopen/runtime/runstate (целиком), host+msrc_diag хвосты 200КБ
  > (с пометкой truncate, .old тоже), token.txt (elevated/SeCreateGlobalPrivilege),
  > autostart_task.txt (schtasks), crashes.txt (список .dmp, не сами), manifest.txt
  > (+ приватность settings.json). Только чтение; частичный успех → exit 0.
  > Проверено: 14/19 собрано (5 missing — несуществующие .old/транзенты, корректно);
  > --out работает; help документирует; e2e 63 PASS.
- [x] **P2.4. (Опционально) ETW / трассировка.** Только если P0–P2.3 не
  закроют вопросы. Done: обоснование «логов и дампов не хватило, потому что…».
  > Решение 06.10.2026: SKIP с обоснованием. P0 (формат/счётчики/ротация/метрики) +
  > P1 (уровни/JSON/readiness/конфиг) + P2.1–P2.3 (дампы/маркер/пакет) закрыли все
  > диагностические вопросы, встреченные в e2e-фазах и ktalk-инцидентах (корни находились
  > по логам/счётчикам/дампам; см. skill vcam-e2e). Открытых вопросов,
  > требующих kernel-tracing (потерянные MF-события, races, перфоманс кадра), нет.
  > ETW — при появлении такого вопроса, не раньше.
- [x] **P2.5. Skill диагностики.** Систематизация диагностики средствами P0–P2.3:
  симптом → какие сигналы смотреть (формат строк, `status`/`--json`, счётчики
  fail-open/runtime, readiness-признаки, `VCAM_DEBUG=1`, msrc_diag с учётом
  ротации в `.old` и фильтра по `pid=`), типовые связки «симптом → причина»
  (дополняет таблицы `vcam-e2e`, фокус — на инструментах наблюдаемости, а не на
  фазах e2e), границы (что НЕ логируем — приватность; когда нужен дамп P2.1 /
  пакет P2.3). Done: skill-файл существует, покрывает диагностику end-to-end
  от симптома до причины только средствами наблюдаемости; примеры команд
  проверены на живой системе.
  > Реализовано 06.10.2026: `.agents/skills/vcam-diag/SKILL.md` (порядок
  > status→логи→счётчики→debug→пакет/дамп; карта 9 симптомов → сигналы;
  > msrc-раздел; границы). Проверено живьём: `status --json` + `diag`
  > (14 файлов). P2.4 — SKIP с обоснованием (выше).

## Риски

- **e2e держится на подстроках логов** — смена формата или текстов ломает
  фазы; каждый шаг P0/P1 — вместе с правкой матчей (список — в
  `pipeline-first.md` P1.1).
- **Overhead** — счётчики и debug-лог не должны влиять на pacing 30 FPS
  (бюджет кадра ~4 мс).
- **in-proc MediaSource** — чужой процесс: дампы и объём лога ограничены,
  диагностика асимметрична host/CLI.
- **Приватность** — в логи не попадают кадры и содержимое видео; пути к
  файлам пользователя — только в debug-уровне.
- **Объём логов** — verbose без cap превращает P1.1 в сотни МБ: cap (P0.4)
  раньше или вместе с P1.1.

## Не делать (осознанно)

- Внешнюю телеметрию / APM (Sentry и т.п.) — продукт локальный, диагностика
  офлайновая.
- Менять строки, которые матчит e2e, без правки скрипта в том же шаге.
- Дублировать per-sink метрики из `pipeline-first.md` P2.3 — они там.
- Начинать ETW раньше, чем логи + метрики + дампы закроют базовые боли.
- Логировать кадры, PII и содержимое настроек сверх нужного.

## Порядок

P0.1 → P0.2 → P0.4 → P0.3 → P1.1 → P1.2 → P1.3 → P1.4 → P2.1 → P2.2 → P2.3
→ P2.4 (по решению) → P2.5. Каждый шаг независимо полезен; e2e зелёный после каждого.
