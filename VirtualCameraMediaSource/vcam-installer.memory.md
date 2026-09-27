# Task Memory: VCam Installer (Inno Setup)

## Version
Current version: `0.0.1` (must be incremented on each release).

## Goal
Create an Inno Setup installer for VCam that performs:
1. Elevated per-machine installation (copies binaries to `C:\Program Files\VCam\`, registers `MediaSource.dll` via `regsvr32 /s`, sets up autostart via `HKCU\Run\VCamAutostart` and `VCamRegistrar`).
2. Robust upgrade support (safe shutdown of `FrameServer` service, handling locked files, ensuring `dst == src` hash/time check).
3. Clean uninstallation (stops services/processes, unregisters COM, cleans registry/files, preserves `%APPDATA%\VCam\settings.json`).
4. Inclusion of production binaries + diagnostics (`CaptureTest`, `VCamProducerCli`, `VCamPreview`, `VCamVideoStreamProducer`, `VCamSettingsUi`, `Registrar`).

## Plan
1. **Step 1**: Build all projects in `Release|x64`, including self-contained build of `VCamSettingsUi`.
2. **Step 2**: Create Inno Setup script (`installer.iss`) covering files, registry, run entries, and Pascal script for pre-install/post-install (stop FrameServer ritual, registration).
3. **Step 3**: Test installation, upgrade, and uninstallation workflow locally.
4. **Step 4**: Verify with `e2e_test.ps1` phases D/E/F.
