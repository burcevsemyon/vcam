---
name: vcam-installer-build
description: >-
  Установка и обновление VCam (Inno Setup): сборка Release|x64, self-contained VCamSettingsUi,
  инсталлятор vcam_installer.iss (версия 0.0.1+), ритуал FrameServer (sc stop/start) для обновления DLL
  без блокировок, COM-регистрация (regsvr32), автозапуск через задачу Task Scheduler\VCamHost
  (ONLOGON + Highest, создаёт vcam_install_task.ps1 через COM Schedule.Service; HKCU\Run — только
  legacy VCamRegistrar), сохранение пользовательских settings.json в %APPDATA%\VCam,
  тихая установка (/VERYSILENT). Use when: сборка установщика, Inno Setup, VCamSetup, обновление DLL в C:\Program Files\VCam,
  автозапуск камеры при старте, FrameServer занят, деинсталляция VCam.
---

# VCam Installer (проектный skill)

Пайплайн сборки, упаковки, установки и обновления виртуальной камеры VCam с использованием **Inno Setup 7**.

## Структура дистрибутива и версии

- **Текущая версия**: `0.0.3` (инкрементируется при каждом релизе в `vcam_installer.iss`
  и `src/VCamVideoStreamProducer/version.rc` — оба места); артефакты в `releases/VCamSetup-<ver>-x64.exe`.
- **Состав продукта (`Release|x64`)**:
  - `MediaSource.dll` → `C:\Program Files\VCam\MediaSource.dll` (фиксированный путь, критично для hash-guard E2E).
  - `VCamVideoStreamProducer.exe` (tray-хост, единственный писатель кадров).
  - `VCamSettingsUi.exe` (C# WinForms, собран **self-contained x64** без внешних зависимостей .NET).
  - `VCamPreview.exe` (плавающее окно предпросмотра).
  - `Registrar.exe` (управление жизненным циклом камеры).
  - `VCamProducerCli.exe` & `CaptureTest.exe` (диагностика и CLI-утилиты).
  - `%APPDATA%\VCam\settings.json` **не поставляется и не перезаписывается** (пользовательские настройки).

## Сборка перед упаковкой

1. **C++ Проекты (Release|x64)**:
   ```bat
   "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" VirtualCameraMediaSource.sln /p:Configuration=Release /p:Platform=x64 /m /v:m /nologo
   ```
2. **C# UI (self-contained x64)**:
   ```powershell
   dotnet publish VirtualCameraMediaSource/src/VCamSettingsUi/VCamSettingsUi.csproj -c Release -r win-x64 --self-contained true /p:PublishSingleFile=true
   ```
3. **Компиляция установщика (Inno Setup 7)**:
   ```bat
   "C:\Program Files\Inno Setup 7\ISCC.exe" VirtualCameraMediaSource/vcam_installer.iss
   ```
   Результат: `VirtualCameraMediaSource/VCamSetup-<ver>-x64.exe` → перенести в `releases/`.

## Меню «Пуск» (`[Icons]`) — ОДИН ярлык

Решение пользователя: много ярлыков отталкивает, всё нужное уже в tray-меню хоста и в UI.

- **«Запуск камеры VCam»** → `{sys}\wscript.exe "{app}\vcam_run_host.vbs"` (IconFilename = хост) —
  единственный ярлык; нужен только
  для запуска после явного выхода (tray → «Выход») или при выключенном автозапуске.
  **НЕ запускать exe напрямую из ярлыка**: не-elevated токен без SeCreateGlobalPrivilege →
  FrameWriter уходит в `Local\`-секцию → svchost FrameServer (session 0) её не видит →
  device-режим (ktalk) без кадров. Задача VCamHost (Highest) даёт elevated-токен и `Global\`
  (проверено: `schtasks /run` из не-elevated шела — без UAC-промта).
  **Не `powershell -WindowStyle Hidden`**: conhost создаётся до парсинга ключа — окно
  мелькает (жалоба юзера); `vcam_run_host.vbs` (WScript, window style 0) не даёт окон.

Остальное (было 3 ярлыка, удалены):
- Настройки / Предпросмотр → **tray-меню хоста** («Настройки VCam…», «Окно предпросмотра…»;
  двойной клик по иконке = Настройки).
- Перезапуск → **кнопка «Перезапустить хост» в VCamSettingsUi** (стоп через Stop-event → ожидание
  graceful-выхода ≤5 с → старт; чистого «Остановить» больше нет — после закрытия UI хост не остаётся
  мёртвым). Явный выход — только tray → «Выход».
- `vcam_restart_host.ps1` остаётся в `{app}` (диагностика/ручной запуск), но ярлыка на него нет.

В tray-меню хоста также есть **«О программе…»** — версия из VERSIONINFO
(`src/VCamVideoStreamProducer/version.rc`, `ProductVersionString()`); при смене версии править
`version.rc` **и** `AppVersion` в `.iss` (оба = 0.0.3).

## Ключевые механизмы инсталлятора (`vcam_installer.iss`)

1. **Ритуал обновления (FrameServer lock)**:
   Так как `MediaSource.dll` загружена в системный процесс `svchost` (сервис `FrameServer`), прямая перезапись файла вызывает блокировку/ошибку. Установщик выполняет:
   - `sc.exe stop FrameServer` (остановка службы перед копированием).
   - Замена файлов в `C:\Program Files\VCam\`.
   - Компонентная регистрация `regsvr32`.
   - `sc.exe start FrameServer` (перезапуск службы; создаётся новый экземпляр svchost).
2. **Автозапуск (Task Scheduler вместо HKCU\Run)**:
   Хост из `HKCU\Run` стартует с UAC-Limited токеном (без `SeCreateGlobalPrivilege`) и пишет в
   `Local\VCam.FrameBuffer.v1`, а сервис MF видит только `Global\` — после ребута раскол секций и
   «нет сигнала». Единственный механизм автозапуска хоста — задача `Task Scheduler\VCamHost`:
   триггер Logon, `RunLevel HighestAvailable` (полный токен при входе) → сразу `Global\`.
   Создаётся в `ssPostInstall` вызовом `powershell -File "{app}\vcam_install_task.ps1"` (скрипт — в
   `[Files]`, рядом с exe): **COM `Schedule.Service`** (`NewTask` → Trigger 9/Logon → Action Path=exe →
   Principal LogonType=3/RunLevel=1 → `RegisterTask(name, $def.XmlText, 6, ...)` — только `XmlText`,
   объект PowerShell ломает overload), Settings — `DisallowStartIfOnBatteries=false` и
   `StopIfGoingOnBatteries=false` (иначе ноут на батарее не поднимет камеру; свойства
   `AllowStartOnBatteries` в ITaskSettings нет). Результат (успех/текст ошибки) —
   `%LOCALAPPDATA%\VCam\setup_task.log`. **Прямой `Exec('schtasks.exe', ...)` из инсталлятора не
   использовать** — стабильно отдаёт 0x80004005, хотя та же команда elevated вручную работает.
   В `ssPostInstall` также чистятся legacy `HKCU\Run\VCamAutostart` (апгрейд ≤0.0.2, иначе двойной
   старт) **и `VCamRegistrar`** (апгрейд ≤0.0.2), в `usUninstall` — `Unregister-ScheduledTask` и оба
   Run-ключа. **`[Registry]`-ключ `VCamRegistrar` и запуск холдера из ssPostInstall удалены в 0.0.3**:
   камеру поднимает сам хост (`StartCameraHolder` при старте, `taskkill Registrar` при tray «Выход»,
   vcam-camera-lifecycle) — после установки хост стартует первым и регистрирует камеру.
3. **Деинсталляция**:
   - Завершение процессов (`VCamVideoStreamProducer`, `Preview`, `SettingsUi`, `Registrar`).
   - Удаление задачи `VCamHost` (`Unregister-ScheduledTask` из powershell) и legacy-ключа Run.
   - Временная остановка FrameServer, отмена регистрации (`regsvr32 /u /s`), запуск FrameServer.
   - Удаление бинарников; `%APPDATA%\VCam\settings.json` сохраняется.

## Проверка после установки

1. `CaptureTest inspect` → `count=2` (VCam присутствует с 3+ медиатипами, `mediaTypes>=3`:
   RGB32/NV12 720p + RGB32 640p + ladder; было ровно 3 — не матчить точное число).
2. `VCamProducerCli status` → `host: running`, `writer section: frames are being written`.
3. Камера видна в приложениях (ktalk, Windows Settings «Камеры»).
4. **Лог хоста**: `%LOCALAPPDATA%\VCam\host.log` — единый формат строк
   `[YYYY-MM-DD HH:MM:SS.mmm] [уровень] [host] сообщение` (P0.1), ротация cap 1МБ →
   `host.log.old` (на старте и в рантайме, P0.4). Читается живьём (`Get-Content -Encoding UTF8`), пока хост пишет
   (открыт с `FILE_SHARE_READ`; не открывать через `fopen` — блокирует чтение). Единственный источник
   правды при старте задачи/из UI (stdout теряется). Строки: `token: elevated=N SeCreateGlobalPrivilege=N`
   (1/2 = полный токен — ожидается от задачи VCamHost), `autostart enabled (Task Scheduler\VCamHost)`,
   `config applied: ...` (что применено), `writer ready (Global\...)`,
   `previous run did not shut down cleanly` (прошлое падение — P2.2).
5. **Задача автозапуска**: `schtasks /Query /TN VCamHost` (+ `/XML` для `<Command>` целиком и
   `RunLevel`), лог создания — `%LOCALAPPDATA%\VCam\setup_task.log`.

## Частичный деплой (только хост)

Если изменился только `VCamVideoStreamProducer.exe` (не `MediaSource.dll`) — **FrameServer НЕ трогать**
(остановка рвёт активную сессию потребителя, ktalk). Ритуал:
`Stop-Process -Name VCamVideoStreamProducer -Force` → elevated-копия exe в `{app}` (к `C:\Program Files\VCam`
не-elevated запись запрещена: UAC → `Start-Process -Verb RunAs`) → сверить SHA256 build vs installed →
запустить хост → проверить `host.log` и `status`.
Полный ритуал с `sc stop FrameServer` нужен **только** при замене `MediaSource.dll` (перед ним закрывать
ktalk, иначе `sc stop` висит в STOP_PENDING).
