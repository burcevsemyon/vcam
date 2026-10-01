---
name: vcam-installer-e2e
description: >-
  E2E-проверка инсталлятора VCam в Hyper-V VM (Win11 «Среда разработки»): установка /VERYSILENT через
  PowerShell Direct, пост-инсталл-чеки (файлы+SHA256, ярлык 1/1 «Запуск камеры VCam», задача
  Task Scheduler\VCamHost + setup_task.log, HKCU Run пуст (VCamRegistrar удалён в 0.0.3), процессы, host.log,
  list-devices rows=0, inspect count=1), ребут-тест с автологоном и верификацией авторанта из задачи
  VCamHost (фикс «после ребута нет сигнала» → token: elevated=1 SeCreateGlobalPrivilege=2 +
  writer ready (Global\...)), cleanup автологона. Use when: проверить
  установщик, установка в VM, reboot-тест VCam, VCamSetup e2e, проверка ярлыков
  и автозапуска после установки, пункт «О программе» в tray.
---

# VCam Installer E2E (проектный skill)

Ритуал проверки инсталлятора **по следам** реального прогона 29.09.2026 (симптом «после ребута нет
сигнала» воспроизведён и закрыт фиксом). Сборка/упаковка — skill `vcam-installer-build`; фазы
камеры/hot-switch — skill `vcam-e2e`.

## Среда и доступ

- **VM**: «Среда разработки под Windows 11» — имя кириллическое, в скриптах выбирать паттерном
  `*Windows 11*`. Гость: юзер `User` (WINDEV2407EVAL, админ), пароль `123` (пустой пароль отвергается —
  «Недопустимые учетные данные»).
- **PowerShell Direct**: `New-PSSession -VMName $vm -Credential $cred` — работает из обычного шелла
  хоста; host-UAC нужен только для операций с `C:\Program Files\` **хоста** (не гостя).
- **Скрипты ASCII-only**: PS5.1 ломает `.ps1` UTF-8 без BOM с кириллицей; вывод гостя с кириллицей
  escape'ить в `\uXXXX` перед печатью в консоль.
- **Инсталлятор** копировать в гость `Copy-Item -ToSession` (например, в `C:\Users\User\`) и запускать
  оттуда: `Start-Process '...\VCamSetup-<ver>-x64.exe' '/VERYSILENT /SUPPRESSMSGBOXES' -Wait`.
  Версии линейки «по порядку релизов»: `0.0.1`, `0.0.2`, `0.0.3` (инкремент — в `vcam_installer.iss`).

## КРИТИЧЕСКИЙ питфол: сеансы 0 vs 2

- `Invoke-Command -VMName` исполняется в **сеансе 0 гостя**; юзер/хост/превью — в **сеансе 2**
  (interactive). Local-namespace сеансов не пересекается.
- Поэтому `VCamProducerCli status` из PS Direct **врёт**: «host not running» при живом хосте и/или
  видит постороннюю `Global\... seq 0`. Это артефакт диагностики, **не баг продукта**.
- **Честный status — только из сеанса юзера** (юзер руками в PowerShell):
  `& "$env:ProgramFiles\VCam\VCamProducerCli.exe" status | Out-File "$env:TEMP\vcam_status.txt"`
  → файл прочитать через PS Direct (на диске он общедоступен). Не путать шеллы: `%TEMP%` в PowerShell
  не подставится, `&` в cmd — тоже.
- `host.log`, файлы, HKCU/процесс-список — из PS Direct читать можно (файл/реестр на диске не «сессионные»).
- Сверка сеансов при расхождении: `(Get-Process -Id $PID).SessionId` (диагностика = 0) vs
  `Get-Process VCamVideoStreamProducer` (хост = 2).

## Автологон (one-shot)

- Перед ребутом в Winlogon (`HKLM\...\Winlogon`, запись из сеанса юзера-админа работает):
  `AutoAdminLogon=1`, `DefaultUserName=User`, `DefaultPassword=123`.
- **Windows гасит автологон после первого автологон-входа** (`AutoAdminLogon=0`, `DefaultPassword`
  стирается) — переставлять **перед каждым** ребутом.
- Если юзер уже на экране логина — автологон не сработает: попросить войти руками (`User`/`123`).
- После теста — **cleanup**: удалить `AutoAdminLogon`, `DefaultUserName` (`DefaultPassword`,
  `AutoLogonCount`, `Alt*` обычно уже отсутствуют).

## Ритуал по фазам

### F0 — Prep
- VM Running; посторонние писатели/читатели в госте остановить (два писателя интерливят кадры —
  правило из `vcam-e2e`), хэш инсталлятора зафиксировать.
- **Предупредить юзера заранее**: в VM будут UAC/ребут/паузы — не нажимать ничего лишнего.

### F1 — Установка
- `/VERYSILENT /SUPPRESSMSGBOXES`, `-Wait`; инсталлятор сам: стоп FrameServer → копирование →
  regsvr32 → старт FrameServer → задача `VCamHost` (через `vcam_install_task.ps1`, log —
  `setup_task.log`) + legacy-чистка `HKCU\Run\VCamAutostart` **и `VCamRegistrar`** → запуск хоста
  (хост сам поднимает холдер `Registrar.exe add VCam hold` — vcam-camera-lifecycle; их токен Full →
  `writer ready (Global\...)` допустим; важно `active` без `(write failed)`).

### F2 — Post-install чеки (из PS Direct)
- **Файлы** в `C:\Program Files\VCam\`: `MediaSource.dll`, `VCamVideoStreamProducer.exe`,
  `VCamSettingsUi.exe`, `VCamPreview.exe`, `Registrar.exe`, `VCamProducerCli.exe`, `CaptureTest.exe`,
  `vcam_restart_host.ps1`, `vcam_install_task.ps1`; SHA256 vs `build\x64\Release` (и vs publish UI).
- **Ярлык 1/1** в `%APPDATA%\Microsoft\Windows\Start Menu\Programs\VCam\`: «Запуск камеры VCam» →
  **`{sys}\wscript.exe "{app}\vcam_run_host.vbs"`**
  (IconFilename = хост; НЕ прямой exe — прямой запуск даёт `Local\`-секцию и слепой device-режим;
  НЕ powershell — conhost мелькает, vbs даёт 0 окон);
  хост (единственный; Настройки/Предпросмотр — в tray-меню хоста, перезапуск — кнопка в UI;
  старых ярлыков «Настройки/Предпросмотр/Перезапуск» в свежей сборке быть не должно).
- **Задача автозапуска**: `schtasks /Query /TN VCamHost` — есть; `/XML`: `<Command>` ЦЕЛИКОМ
  `C:\Program Files\VCam\VCamVideoStreamProducer.exe` (без разрыва на `C:\Program`+`Files\...`),
  `RunLevel HighestAvailable`, `<LogonTrigger>`; `%LOCALAPPDATA%\VCam\setup_task.log` = «created: \VCamHost».
  **`HKCU\Run`**: **пусто** — `VCamAutostart` устарел и удаляется, `VCamRegistrar` удалён в 0.0.3
  (камеру поднимает хост; наличие `VCamRegistrar` = апгрейд со старой версии, ключ должен быть
  удалён ssPostInstall).
- **Процессы**: `Registrar` (хост-холдер), `VCamVideoStreamProducer`.
- **host.log** (`%LOCALAPPDATA%\VCam\host.log`): `starting → autostart → tray → camera holder started
  → watching → writer ready (...) → switch → source opened → active`. Читается живьём
  (`Get-Content -Encoding UTF8`). Строка `camera holder started` = хост зарегистрировал камеру
  (0.0.3+); при tray «Выход» ожидается `camera holder stopped`.
- **`CaptureTest inspect` → `count=1`** в VM (физ. камер нет; `count=2` — для рабочей машины).
- **`VCamProducerCli list-devices` → `rows=0`** — фильтр `IsVirtualCamera` скрывает нашу камеру
  (в VM больше камер и нет — rows=0 = SUCCESS, не провал).

### F3 — Ребут
1. Переставить автологон (см. выше) → `Restart-Computer -Force`.
2. Ждать: сессия недоступна → probe каждые ~10 с (`Get-Process VCamVideoStreamProducer` +
   `explorer`); автологон+старт ≈ 30–60 с.
3. Сразу после входа зафиксировать состояние Winlogon (one-shot сброшен — норма).

### F4 — Верификация post-reboot (главная фаза)
- **host.log НЕ должен содержать** `writer open failed: CreateFileMappingW failed: 5` (исходный
  баг: автозапуск из HKCU\Run давал UAC-filtered токен **без SeCreateGlobalPrivilege**).
- Ожидается: первая же строка токена — `token: elevated=1 SeCreateGlobalPrivilege=2` (хост от
  задачи VCamHost, Highest) и `shared memory writer ready (Global\VCam.FrameBuffer.v1)`
  (фолбэк `Create(Global) → Open(Global) → Create(Local)` остаётся, но при Elevated создастся Global)
  и `active` без `(write failed)`. **`writer ready (Local\...)` после ребута = провал** (задача
  упала на Non-Highest/токен остался Limited).
- **status из сеанса юзера** (юзером, см. «сеансы»): `host VCamVideoStreamProducer: running`,
  `Local\VCam.FrameBuffer.v1: open, frames are being written (seq N -> M)` — seq должен **расти**
  (два замера с паузой).
- **Превью**: юзер открывает из Пуска → картинка (settings.json — static green BMP). «Нет сигнала»
  первые 1–3 с допустимо: self-heal в коде (retry 1 с, reconnect 3 с) — переждать/переоткрыть.

### F5 — Cleanup
- Удалить остатки автологона (Winlogon: `AutoAdminLogon`, `DefaultUserName`, и `DefaultPassword`/
  `AutoLogonCount`/`Alt*` — если есть).
- Опционально: остановить хост/удалить тестовые файлы, вернуть settings.json из бэкапа (если
  сохранялся — см. `vcam-e2e`).

## Сигнатуры SUCCESS / ПРОВАЛ

| Проверка | SUCCESS | ПРОВАЛ |
|---|---|---|
| установка | `/VERYSILENT` exit ok, файлы+hash | UAC-таймаут, locked exe (закрыть приложения) |
| ярлыки (VM) | ровно 1: «Запуск камеры VCam» | старые ярлыки/3 шт. — старый пакет |
| post-install host.log | `writer ready`, `active` | `CreateFileMappingW failed: 5` (в сборке нет фикса) |
| list-devices (VM) | `rows=0` | видит `VCam (` — фильтр не работает |
| inspect (VM) | `count=1` | 0 — Registrar не поднял камеру |
| post-reboot host.log | `token: elevated=1 ...=2`, `writer ready (Global\...)` | `writer open failed: ... 5`, `writer ready (Local\...)`, `token: elevated=0` |
| status (сеанс юзера) | `frames are being written`, seq растёт | `no new frames (seq 0)`, `not running` |
| превью | картинка | NO SIGNAL дольше ~5 с после reconnect |

## Диагностика отклонений

1. `host.log` (tail, UTF-8, живьём) — первоисточник; stdout при тихом старте (задача/UI) теряется.
   Ключевые строки: `token: elevated=N SeCreateGlobalPrivilege=N` (0 = Limited-токен, баг Run;
   1/2 = задача Highest — норма), `autostart enabled (Task Scheduler\VCamHost)`, `writer ready (Global\...)`.
2. Session-id сверка (0 vs 2) — объясняет «ложный» status.
3. Посторонняя `Global\... seq 0`: её создаёт процесс с привилегией (обычно наша же диагностика из
   сеанса 0: `inspect`/probe). Читатели перебирают Global→Local и встают на мусорную → убить источник,
   переоткрыть status/превью. Если «пустышку» держит живой потребитель — секция не умрёт сама.
4. Application/System-лог гостя, WER (падений VCam быть не должно).
5. `msrc_diag.log` в госте по умолчанию отсутствует — не искать, если не включался.

## Память задач

`vcam-installer.memory.md`, `memory.md` (в `VirtualCameraMediaSource/`) — **локальные, в git не идут**
(`.gitignore`: `memory.md`, `*.memory.md`); хранят факты прогона, фиксы и питфолы.
