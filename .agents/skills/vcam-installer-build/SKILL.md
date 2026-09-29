---
name: vcam-installer-build
description: >-
  Установка и обновление VCam (Inno Setup): сборка Release|x64, self-contained VCamSettingsUi,
  инсталлятор vcam_installer.iss (версия 0.0.1+), ритуал FrameServer (sc stop/start) для обновления DLL
  без блокировок, COM-регистрация (regsvr32), автозапуск через HKCU\Run (VCamAutostart + VCamRegistrar
  скриптдержатель Registrar.exe add VCam hold), сохранение пользовательских settings.json в %APPDATA%\VCam,
  тихая установка (/VERYSILENT). Use when: сборка установщика, Inno Setup, VCamSetup, обновление DLL в C:\Program Files\VCam,
  автозапуск камеры при старте, FrameServer занят, деинсталляция VCam.
---

# VCam Installer (проектный skill)

Пайплайн сборки, упаковки, установки и обновления виртуальной камеры VCam с использованием **Inno Setup 7**.

## Структура дистрибутива и версии

- **Текущая версия**: `0.0.2` (инкрементируется при каждом релизе в `vcam_installer.iss` и
  `vcam-installer.memory.md`); артефакты в `releases/VCamSetup-<ver>-x64.exe`.
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

- **«Запуск камеры VCam»** → `{app}\VCamVideoStreamProducer.exe` — единственный ярлык; нужен только
  для запуска после явного выхода (tray → «Выход») или при выключенном автозапуске.

Остальное (было 3 ярлыка, удалены):
- Настройки / Предпросмотр → **tray-меню хоста** («Настройки VCam…», «Окно предпросмотра…»;
  двойной клик по иконке = Настройки).
- Перезапуск → **кнопка «Перезапустить хост» в VCamSettingsUi** (стоп через Stop-event → ожидание
  graceful-выхода ≤5 с → старт; чистого «Остановить» больше нет — после закрытия UI хост не остаётся
  мёртвым). Явный выход — только tray → «Выход».
- `vcam_restart_host.ps1` остаётся в `{app}` (диагностика/ручной запуск), но ярлыка на него нет.

В tray-меню хоста также есть **«О программе…»** — версия из VERSIONINFO
(`src/VCamVideoStreamProducer/version.rc`, `ProductVersionString()`); при смене версии править
`version.rc` **и** `AppVersion` в `.iss` (оба = 0.0.2).

## Ключевые механизмы инсталлятора (`vcam_installer.iss`)

1. **Ритуал обновления (FrameServer lock)**:
   Так как `MediaSource.dll` загружена в системный процесс `svchost` (сервис `FrameServer`), прямая перезапись файла вызывает блокировку/ошибку. Установщик выполняет:
   - `sc.exe stop FrameServer` (остановка службы перед копированием).
   - Замена файлов в `C:\Program Files\VCam\`.
   - Компонентная регистрация `regsvr32`.
   - `sc.exe start FrameServer` (перезапуск службы; создаётся новый экземпляр svchost).
2. **Автозапуск (Session-lifetime / Рестарт)**:
   Виртуальная камера имеет сессионное время жизни (исчезает при завершении регистратора). Для автоподнятия после перезагрузки и логина используются ключи `HKCU\Software\Microsoft\Windows\CurrentVersion\Run`:
   - `VCamAutostart`: запускает tray-хост `VCamVideoStreamProducer.exe`.
   - `VCamRegistrar`: запускает холдер скрыто через PowerShell:
     `powershell.exe -WindowStyle Hidden -Command "Start-Process -FilePath '{app}\Registrar.exe' -ArgumentList 'add','VCam','hold' -WindowStyle Hidden"`.
   После установки скрипт немедленно стартует холдер и хост, чтобы камера была доступна сразу.
3. **Деинсталляция**:
   - Завершение процессов (`VCamVideoStreamProducer`, `Preview`, `SettingsUi`, `Registrar`).
   - Временная остановка FrameServer, отмена регистрации (`regsvr32 /u /s`), запуск FrameServer.
   - Удаление бинарников и ключей автозапуска в `HKCU\Run`.
   - **Сохранение** `%APPDATA%\VCam\settings.json` (пользовательские данные).

## Проверка после установки

1. `CaptureTest inspect` → `count=2` (VCam присутствует с 3 медиатипами: NV12 + RGB32 720p/480p).
2. `VCamProducerCli status` → `host: running`, `writer section: frames are being written`.
3. Камера видна в приложениях (ktalk, Windows Settings «Камеры»).
4. **Лог хоста**: `%LOCALAPPDATA%\VCam\host.log` — строки с таймстампом `[HH:MM:SS.mmm]`,
   ротация >1 МБ → `host.log.old`. Читается живьём (`Get-Content -Encoding UTF8`), пока хост пишет
   (открыт с `FILE_SHARE_READ`; не открывать через `fopen` — блокирует чтение). Единственный источник
   правды при старте из `HKCU\Run` (stdout теряется).

## Частичный деплой (только хост)

Если изменился только `VCamVideoStreamProducer.exe` (не `MediaSource.dll`) — **FrameServer НЕ трогать**
(остановка рвёт активную сессию потребителя, ktalk). Ритуал:
`Stop-Process -Name VCamVideoStreamProducer -Force` → elevated-копия exe в `{app}` (к `C:\Program Files\VCam`
не-elevated запись запрещена: UAC → `Start-Process -Verb RunAs`) → сверить SHA256 build vs installed →
запустить хост → проверить `host.log` и `status`.
Полный ритуал с `sc stop FrameServer` нужен **только** при замене `MediaSource.dll` (перед ним закрывать
ktalk, иначе `sc stop` висит в STOP_PENDING).
