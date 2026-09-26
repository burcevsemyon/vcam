# vcam-producer-cli (subagent 3) — VCamProducerCli + e2e + README

Задача: этап 3/3 — CLI (`run`/`status`) для отладки/e2e, обновлённый e2e_test.ps1, README.
Ветка: feature/vcam-video-stream-producer. Память: vcam-producer.memory.md (дизайн) +
vcam-producer-tray.memory.md (хост) + vcam-producer-core.memory.md (ProducerCore API) +
vcam-video.memory.md (MSBuild/E2E/контракт).
Лимит: 2 раунда (раунд = сборка→правка→перетест).

## Статус
- [x] Раунд 1: код (CLI, vcxproj, .sln, e2e, README) → сборка exit 0 с первого раза.
- [x] Раунд 2: фикс product-бага парсера ProducerCore → e2e SUCCESS (0 failures, exit 0).
- [x] Приёмка 1–5 выполнены. DONE. Коммит НЕ делался.

## Факты
- Создано: `src/VCamProducerCli/VCamProducerCli.cpp` (run/status, Emit UTF-8/WriteConsoleW,
  Esc+CtrlHandler, HostRunning по мьютексу, status seq-проба 300 мс) и
  `VCamProducerCli.vcxproj` (GUID {81302D8C-7CB8-4E2C-9224-B1BA3947C47A}); в .sln
  project-блок + 2 строки Release|x64 (LF/табы сохранены).
- Сборка `"...\MSBuild.exe" VirtualCameraMediaSource.sln /p:Configuration=Release
  /p:Platform=x64 /m` → exit 0 (каждый прогон).
- Баг №1 (CLI): `%APPDATA%` в format-строке help → invalid parameter handler →
  exit 0xC0000409. Фикс `%%APPDATA%%`.
- Баг №2 (product, ProducerCore `Settings.cpp` FindObjectRange): `find("\"video\"")`
  первым матчил **значение** `"type": "video"`, следующий `:` брался у секции
  `"static"` → при `source.type=video` читалась static-секция: `video.path = static.path`
  (симптом: e2e phase B кадры идентичны, `status` показывал video.path от static;
  для `type=static` баг маскировался — совпадение давало правильную секцию случайно).
  Фикс: `FindKeyPos` — после ключа обязан следовать `:`. Затрагивает и tray-хост
  (общая библиотека). Проверен probe-ом: status печатает test_video.mp4.
- `e2e_test.ps1` переписан: бэкап settings → фаза A `run --type video --path
  e2e_output\test_video.mp4` (движение unique=4..5, nonBlack, writer ready/active в
  логе) → фаза B `run` без overrides (запись новой схемы static → кадры идентичны →
  запись video → кадры различаются, ≥2 `[cli] switch:` + `source opened: type=video`)
  → restore байт-в-байт (SHA256 match) → exit 0, 0 failures.
- Приёмка 2: Ctrl+C — child powershell (`SetConsoleCtrlHandler(NULL,TRUE)` ignore →
  `AttachConsole(cliPid)` → `GenerateConsoleCtrlEvent(0,0)` → handle OpenProcess
  читает exit code): stdout `ATTACHED SENT CLI_EXIT=0`, helper exit 0, CLI чисто вышел
  (exit 0). CLI запускается через `start "" /min` — собственная консоль, сигнал не
  задевает нашу.
- Приёмка 3: `status` exit 0, legacy-схема: migrated on read / file untouched.
- Приёмка 5: README — состав (+ProducerCore, +хост, +CLI; StaticProducer/VideoProducer
  помечены legacy; UI = `.exe`, рядом managed `.dll` — MSBuild-лог пишет `.dll`),
  деплой (хост основной, CLI для отладки/e2e), новая схема settings + таблица,
  autostart = HKCU\Run, файлы. Контракт памяти/регистрация/исторический баг сохранены.
- Среда: settings.json восстановлен (legacy, SHA256 ✓); процессов VCam не осталось;
  камера зарегистрирована (не трогали).

## Риски
- Hot-switch с `--type`/`--path` overrides по дизайну игнорирует правку этого
  параметра в файле — e2e phase B проверял hot-switch без overrides (честный путь).
- Tray-хост после фикса FindKeyPos не прогонялся (ядро общее) — рекомендуется
  ручная проверка хоста с `source.type=video`.
- Фикс применён только к FindObjectRange; JsonGetString/GetInt/GetBool имеют тот же
  паттерн «ключ+двоеточие», но ищутся внутри фрагментов — риск минимален, не трогал
  (минимальный диф).
- Esc-тест вручную не проводился (код унаследован от StaticProducer); проверен Ctrl+C.
- e2e затирает BMP в `e2e_output` (тестовые артефакты, `test_video.mp4` не трогает).

## Запрос
Нет. Готово к ревью; коммит — по явному разрешению.
