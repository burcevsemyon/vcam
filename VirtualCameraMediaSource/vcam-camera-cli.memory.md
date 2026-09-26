# vcam-camera-cli (subagent B — этап B) — CLI list-devices/--device + e2e фаза C

Задача: этап B плана `vcam-camera.memory.md` — ТОЛЬКО VCamProducerCli + e2e_test.ps1
(UI/README/хост/ProducerCore не трогались). Ветка: feature/vcam-camera-source.
Память вышестоящей: vcam-camera.memory.md + vcam-camera-core.memory.md (API этапа A) +
vcam-producer.memory.md + vcam-producer-cli.memory.md. Лимит 2 раунда.

## Статус
- **DONE** — 1 раунд (без правок после первого прогона: сборка exit 0 с первого раза,
  живые проверки и e2e — с первого раза). Коммит НЕ делался.

## Изменения
### `src/VCamProducerCli/VCamProducerCli.cpp`
- **Команда `list-devices`** (Options.listDevices, CmdListDevices): stdout = строки
  `<id>\t<name>` (одно устройство на строку, UTF-8 через существующий Emit),
  шапка `[cli] N camera device(s) - stdout rows: <id>\t<name>` и сообщение об отсутствии
  устройств — **в stderr** (не ломают парсинг stdout). Таб/CR/LF в id/name заменяются
  на пробел. 0 устройств → пустой stdout + stderr-сообщение, **exit 0**.
- **`--device <id>`** (отдельный флаг, не alias --path): валидации wmain: только с
  `--type camera` (иначе ошибка exit 2), взаимоисключим с `--path`, пустое значение —
  ошибка. Внутри сливается в `g_fixPath` + `g_fixDevice=true`; `ResolveTarget()` после
  override пути делает `want.camName.clear()` — **критично**: без очистки несовпадающий
  id матчился бы по имени из секции camera (открылась бы чужая камера; проверено:
  `--device \\?\nonexistent` при `cam.name="Brio 90"` в settings → NO SIGNAL, не active).
- `--type` принимает `static|video|camera`.
- **Метка логов `TargetLabel(cfg)`**: для `type=camera` — `camName`, если не пусто,
  иначе `path`; применяется в `switch`/`source opened`/`active`/`signal restored`
  (поля `path=%s`, значение — метка). `status`: добавлены строки `camera.id`,
  `camera.name` (как static/video), плюс `camera.source:` (метка) ТОЛЬКО при
  `source.type=camera`. Хост VCamVideoStreamProducer.cpp НЕ трогался (отдельный шаг).
- `LooksNewSchema`: ключи `"source"/"static"/"video"` + **`"camera"`**.
- Help: команды, `--device`, list-devices-контракт; неизвестная команда → help + exit 2
  (существующее поведение сохранено); message «expected command 'run', 'status' or
  'list-devices'».

### `e2e_test.ps1` — фаза C (=== 6, после B, ДО finally/restore; Result === 7)
- `list-devices` (Start-Process → raw UTF-8 файл → `Get-Content -Encoding UTF8` →
  строки с tab, `$tab -gt 0`) → 0 устройств → `Pass("SKIP: no camera device")`,
  к restore; exit e2e остаётся 0.
- Позитив: `run --type camera --device <первый id>` → poll ≤12 с: `writer ready`
  (Fail, если нет — независим от камеры), `source opened: type=camera` + `active`;
  seq через `& $Cli status` → «frames are being written» (status сам спит 300 мс).
  Пиксели (`Capture 3 e2e_cam` + nonBlack/unique) — **только INFO жёлтым, не FAIL**.
  Не достигнуты opened/active → NOTE с первыми 5 строками open failed/no signal +
  `Pass("SKIP: phase C camera unavailable ...")` — камера занята не роняет e2e.
- Негатив: `run --type camera --device \\?\nonexistent` → sleep 7 с → в логе
  `no signal` И НЕТ `active` И ≥1 `open failed` (ретраи) → Pass; иначе Fail
  (ранний выход CLI тоже Fail). Стоп — Stop-Cli (kill) как в фазах A/B.
- Порядок бэкап/restore settings SHA256 не менялся; BMP `e2e_output` затираются,
  `test_video*.mp4` не трогаются.

## ФОРМАТ list-devices (контракт для UI-субагента, subagent C)
- **stdout**: только строки `<id>\t<name>` (одна строка = одно устройство), UTF-8,
  `\t` = разделитель; id = MF symlink (может содержать `#&{}?`), name = friendly name;
  пустых id/name в строке нет (tab/CR/LF в имени вычищаются в пробел).
- **stderr**: шапка/сообщения («N camera device(s)…», «no camera devices found (0)») —
  stdout-парсер их игнорирует. Exit всегда 0 (в т.ч. 0 устройств).
- Парсить ТОЛЬКО stdout (например `Get-Content -Encoding UTF8`, split по "`t",
  первая part = id); `2>&1` в парсер НЕ сливать.

## Результаты (приёмка)
- Сборка MSBuild Release|x64 → **exit 0** (2 раза: вручную + внутри e2e).
- `list-devices`: строка `\\?\usb#vid_046d&pid_0949&mi_00#6&466a1bd&0&0000#{e5323777-f976-4f5b-9b55-b94699c46e44}\global\tBrio 90`;
  консоль exit 0; пайп-файл: BOM нет (первые байты 92,92,63), U+FFFD нет, stderr = шапка.
- `run --type camera --device <id>`: opened+active за ~1 с; status seq 12→30 за 300 мс;
  лог содержит writer ready / switch / source opened / active; Ctrl+C (attach к
  консоли CLI + GenerateConsoleCtrlEvent) → наблюдатель Win32-хэндла: **EXIT_CODE=0**,
  лог заканчивается «writer closed / exit».
- `run --type camera --device \\?\nonexistent`: `no signal` ~5 с (в окне 7 с),
  **19** «open failed»-ретраев, active отсутствует, чистый выход; при cam.name="Brio 90"
  в settings — тоже NO SIGNAL (camName очищается ✓).
- `status --settings <temp>` (type=camera, name="Brio 90"): `camera.source: Brio 90`;
  `run --type camera --settings <temp>` (без --device): switch/opened/active печатают
  `path=Brio 90` (метка = camName) ✓.
- **e2e: exit 0, 0 failures** — фаза A (PASS), B (PASS), C: 1 устройство → writer ready
  PASS, opened+active PASS, seq PASS, INFO pixels nonBlack=1200 unique=2 (не FAIL),
  негатив PASS (retries=19), settings SHA256 restore PASS. SKIP-ветка камеры не
  срабатывала (камера доступна).
- `git status`: M e2e_test.ps1, M VCamProducerCli.cpp (+ файлы этапа A) — как ожидалось.
  Процессов VCam/CaptureTest не осталось; settings.json пользователя e2e восстановил.

## Питфолы (для следующих)
- **PS5.1-квирк**: `Process.ExitCode` у объекта `Start-Process -PassThru` с
  `-RedirectStandardOutput` возвращает пусто (даже после WaitForExit=true); обход —
  Win32-наблюдатель (`OpenProcess(SYNCHRONIZE|QUERY_LIMITED) + WaitForSingleObject +
  GetExitCodeProcess` в отдельном powershell, temp\opencode\vcam_exit_watcher.ps1).
  `Get-Process`-объект ExitCode тоже пуст; `Stop-Process -Force` даёт -1.
- Отправка Ctrl+C тестовому CLI: отдельный powershell → ignore ctrl → `FreeConsole` →
  `AttachConsole(cliPid)` → проверить `GetConsoleProcessList` (не шлём, если консоль
  общая с обёрткой!) → `GenerateConsoleCtrlEvent(0,0)` (temp\opencode\vcam_ctrlc.ps1).
  У обёртки opencode-тулза консоль ЕСТЬ (GetConsoleWindow=0, но GetConsoleProcessList
  непустой, pids общие); дети от `Start-Process` (в т.ч. с -Redirect*) получают СВОЮ
  консоль (n=1) — событие безопасно.
- Инструмент может убить процесс-дерево вызова, если после завершения команды остаются
  висячие потомки → все прогоны с CLI делать одним вызовом «запуск → проверки → стоп».
  Один ранний вызов был убит (остался 1 лишний CLI — вычищен в следующем прогоне).
- Mojibake кириллицы в логах при `Get-Content` без `-Encoding UTF8` — это ANSI-декод
  UTF-8 файла (логи пишутся UTF-8); в e2e матчи только ASCII.

## Риски
- `list-devices`/камерные прогоны писались при неработающем tray-хосте; с запущенным
  хостом два писателя интерливят кадры (штатное предупреждение CLI, e2e предупреждает).
- Фаза C при недоступной камере даёт SKIP (по дизайну) — реальный баг открытия камеры
  в этой ветке e2e не поймает (поймают негативная проверка + ручной прогон).
- Метка в `switch/active` для camera без camName показывает длинный symlink (так и
  задумано: «camName если не пусто, иначе path») — при настройке через UI camName будет.
- `--path` с `--type camera` camName НЕ очищает (только `--device`) — в help явно
  указано использовать `--device`.

## Запрос
Не требуется. Следующим: subagent C (UI), затем основной (README/хост-логи).
