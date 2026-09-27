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

- **Текущая версия**: `0.0.1` (инкрементируется при каждом релизе в `vcam_installer.iss` и `vcam-installer.memory.md`).
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
   Результат: `releases/VCamSetup-0.0.1-x64.exe`.

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
