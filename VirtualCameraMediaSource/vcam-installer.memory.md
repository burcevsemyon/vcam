# Task Memory: VCam Installer (Inno Setup)

## Version
Current version: `0.0.3` (must be incremented on each release).
Artifacts: `releases/VCamSetup-<ver>-x64.exe` (0.0.1/0.0.2 kept for history).

## Goal
Create an Inno Setup installer for VCam that performs:
1. Elevated per-machine installation (copies binaries to `C:\Program Files\VCam\`, registers `MediaSource.dll` via `regsvr32 /s`, sets up autostart via `HKCU\Run\VCamAutostart` and `VCamRegistrar`).
2. Robust upgrade support (safe shutdown of `FrameServer` service, handling locked files, ensuring `dst == src` hash/time check).
3. Clean uninstallation (stops services/processes, unregisters COM, cleans registry/files, preserves `%APPDATA%\VCam\settings.json`).
4. Inclusion of production binaries + diagnostics (`CaptureTest`, `VCamProducerCli`, `VCamPreview`, `VCamVideoStreamProducer`, `VCamSettingsUi`, `Registrar`).
5. **Start menu shortcuts** (`[Icons]`, `{userprograms}\VCam\`): Настройки VCam, Предпросмотр VCam, **Перезапуск камеры VCam** (скрытый PowerShell `vcam_restart_host.ps1` → Stop-event → ожидание до 20 с → force-fallback → старт хоста).

## Plan
1. **Step 1**: Build all projects in `Release|x64`, including self-contained build of `VCamSettingsUi`.
2. **Step 2**: Create Inno Setup script (`installer.iss`) covering files, registry, run entries, icons, and Pascal script for pre-install/post-install (stop FrameServer ritual, registration).
3. **Step 3**: Test installation, upgrade, and uninstallation workflow locally / in Hyper-V VM.
4. **Step 4**: Verify with `e2e_test.ps1` phases D/E/F.

## Changelog
- **0.0.3** (29.09.2026): виртуальная камера VCam исключена из списка источников «камера»
  (`CameraDevices.cpp IsVirtualCamera` — фильтр в `EnumerateCameraDevices`, CLI+UI).
- **0.0.2** (29.09.2026): файловый лог хоста `%LOCALAPPDATA%\VCam\host.log` (в пакете);
  `[Icons]` — ярлыки в меню «Пуск» + `vcam_restart_host.ps1` (перезапуск хоста).
- **0.0.1** (27.09.2026): первичная сборка.

## Open
- [ ] Прогнать в Hyper-V VM (Win11 «Среда разработки»): установка → ребут → `host.log`;
  там же появятся ярлыки. Локально 0.0.2 НЕ устанавливался (стоп FrameServer рвёт ktalk).
