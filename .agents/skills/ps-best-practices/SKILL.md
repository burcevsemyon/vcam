---
name: ps-best-practices
description: >-
  PowerShell best-practices для скриптов (фокус 5.1, пометки про 7): кодировки
  (BOM для .ps1 с не-ASCII, без BOM для данных), строгий режим и ошибки, вызов
  native exe (&, массивы аргументов, LASTEXITCODE), Start-Process паттерны,
  грабли RedirectStandardOutput/ExitCode, чтение файлов с явной кодировкой,
  атомарная запись tmp+rename, реестр HKCU/HKLM и elevation, polling с дедлайном.
  Use when: пишешь или ревьюишь .ps1, скрипт падает/ведёт себя странно, вызов
  exe из скрипта, парсинг вывода, работа с файлами/реестром, elevation.
---

# Skill: PowerShell best-practices (фокус 5.1)

## 0. Версия и строгость

- Знай, под чем идёшь: `powershell.exe` = 5.1, `pwsh` = 7+. Цепочки `&&`/`||`,
  тернарные и прочие прелести 7 в 5.1 — синтаксическая ошибка. Если скрипт должен
  ходить везде — пиши на подмножестве 5.1.
- В начале скрипта: `Set-StrictMode -Version Latest` (ловит опечатки в переменных)
  и осознанный `$ErrorActionPreference` (`Stop` для fail-fast в скриптах-оркестраторах,
  иначе non-terminating ошибки тихо продолжаются).
- Функции — маленькие, с типизированными параметрами (`[string]$x`, `[int]$TimeoutSec = 10`),
  возвращают значение через `return`, мусор в pipeline не сыплют (осторожно с `Write-Output`
  внутри — всё несвязанное станет частью возврата).

## 1. Кодировки

- `.ps1` с не-ASCII (кириллица) — **UTF-8 с BOM**, иначе парсер 5.1 ломает строки.
- Файлы данных/конфиги/JSON — **UTF-8 без BOM**: `Set-Content -Encoding UTF8` даёт BOM,
  не использовать для данных. Пиши явно:
  `[IO.File]::WriteAllText($p, $s, (New-Object Text.UTF8Encoding($false)))`.
- Чтение — с явной кодировкой, не надейся на автоопределение:
  `[IO.File]::ReadAllText($p, [Text.Encoding]::UTF8)`. Консольный вывод 5.1 —
  системная кодовая страница: кракозябры в консоли — это отображение, байты проверяй явно.

## 2. Вызов native exe

- Оператор `&` + **массив аргументов**, не строка: `& $Exe 'run' '--type' 'static'`.
  Строка с пробелами/кавычками парсится криво — массив всегда точен.
- Код возврата — `$LASTEXITCODE` сразу после вызова (перезаписывается каждой командой).
  Сохраняется и через `| Out-Null` / `| Out-String`. Захват вывода без потери кода:
  `$out = ((& $Exe status --ready 2>&1) | Out-String).Trim()`.
- `2>&1` подмешивает stderr в stdout (объекты ErrorRecord, не строки) — для парсинга
  приводи через `Out-String` + `.Trim()`, для логов пиши как есть.
- Тяжёлый вывод построчно не обрабатывай в цикле PowerShell (`foreach` + `Select-String`
  по мегабайтам — медленно); фильтруй на стороне (`Select-String -Pattern -Path file`).

## 3. Start-Process паттерны

- Фон: `Start-Process -FilePath $Exe -ArgumentList @(...) -PassThru -WindowStyle Hidden
  -RedirectStandardOutput $log -RedirectStandardError $err -NoNewWindow` (консольные),
  `-WindowStyle Hidden` для GUI. Живость — `$proc.HasExited` / `$proc.Id`.
- Остановка: `Stop-Process -Id $proc.Id -Force` + `$proc.WaitForExit(5000)`.
- **Грабли `ExitCode`**: при `-RedirectStandardOutput` свойство `Process.ExitCode`
  пусто — читай `$proc.ExitCode` ТОЛЬКО после `WaitForExit()`. Для быстрых exe проще
  прямой `&` + `$LASTEXITCODE`.
- `-Wait` ждёт ВСЁ дерево процесса (вечный holder повесит шелл) — долгие/вечные
  процессы без `-Wait`, живость проверять отдельно. `-Verb RunAs` + `-Wait` — та же ловушка.
- `UseNewEnvironment` даёт ЧИСТОЕ окружение (без родительских переменных) — для проброса
  env-флагов в дочерний процесс его НЕ ставить (наследование по умолчанию).

## 4. Файлы: чтение, запись, атомарность

- Пути — `Join-Path` (не конкатенация строк), существование — `Test-Path` перед чтением.
- Атомарная запись конфигов/состояния: пиши в `file.tmp`, потом `Move-Item -Force`
  (аналог tmp+rename в C++). Читатель никогда не видит половинку.
- Размер/время файла: `(Get-Item $p).Length`, `.LastWriteTime`. Хвост лога:
  `Get-Content $log -Tail N`. Подстроки в файле: `Select-String -Path $log -Pattern '...' -Quiet`
  для булева ответа (быстро, без загрузки всего файла в память).
- Удаление с игнором отсутствия: `Remove-Item ... -ErrorAction SilentlyContinue`.

## 5. Polling с дедлайном (вместо фиксированных пауз)

- Готовность — опросом кода возврата до дедлайна, не `Start-Sleep N`:
  ```powershell
  $deadline = (Get-Date).AddSeconds(10)
  for (;;) {
      & $Exe status --ready | Out-Null
      if ($LASTEXITCODE -eq 0) { break }
      if ((Get-Date) -ge $deadline) { throw "not ready in 10 s" }
      Start-Sleep -Milliseconds 500
  }
  ```
- Интервал опроса >> длительности одной проверки (500 мс при проверке ~100 мс).
- Не гейтятся опросом: смена состояния (hot-switch), тайминги фолбэков (их тестируем),
  debounce после записи конфига (см. Wait-Ready skill).

## 6. Реестр и elevation

- Чтение: `Get-ItemProperty -Path $key -Name $value -ErrorAction SilentlyContinue`
  (нет ключа = $null, не ошибка). Запись HKLM требует elevation — из plain шелла
  получишь молчаливый no-op/отказ; для тестов используй HKCU (часто приоритетнее).
- После изменения реестра, влияющего на COM/службы, нужен рестарт потребителя
  (перечитать процесс), иначе тестируешь старое состояние.
- Elevation — отдельным шагом с явным запросом; elevated-процессы гаси из elevated
  шелла (из plain `Stop-Process` молча не срабатывает).

## 7. Мини-чеклист ревью .ps1

1. Работает под 5.1? (нет `&&`, `??`, новых операторов).
2. `Set-StrictMode`? Все переменные инициализированы?
3. Кодировки: скрипт с BOM (если не-ASCII), данные без BOM?
4. Каждый `& $Exe` — массив аргументов; `$LASTEXITCODE` проверен там, где важен?
5. `Start-Process` без `-Wait` для долгоживущих; `ExitCode` только после `WaitForExit()`?
6. Паузы — это readiness-poll с дедлайном, а не слепое `Sleep N`? (где применимо)
7. Временные файлы/ключи реестра чистятся в `finally`?
8. Пути через `Join-Path`, секреты не светятся в логах/аргументах (`ps` виден всем)?
