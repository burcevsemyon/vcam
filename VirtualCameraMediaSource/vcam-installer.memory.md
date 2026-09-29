# Task Memory: VCam Installer (Inno Setup)

## Version
Current version: `0.0.2` (must be incremented on each release).
Artifacts: `releases/VCamSetup-<ver>-x64.exe` (0.0.1 kept for history).
**Нумерация «по порядку релизов» (решение пользователя, 29.09)**: реальных релиза два — 0.0.1 и 0.0.2;
промежуточные сборки 0.0.3/0.0.4 (29.09, фильтр камеры / фикс ребута) в эту линейку не попали и
удалены из `releases/`, всё их содержимое входит в релиз 0.0.2 (пересобран 29.09 21:13).

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
- **0.0.2** (29.09.2026, пересобран 21:13): суммарный релиз дня — файловый лог хоста
  `%LOCALAPPDATA%\VCam\host.log`; `[Icons]` — ярлыки в меню «Пуск» + `vcam_restart_host.ps1`;
  виртуальная камера исключена из списка источников «камера» (`IsVirtualCamera`);
  фикс «после ребута нет сигнала» — writer-цепочка `Create(Global) → Open(Global) →
  Create(Local\<base>)` (`FrameWriter::Open` + `SectionOpenedAs`), фактическое имя секции
  в логе хоста и в CLI `status` (перебор префиксов).
  (Внутри дня были промежуточные сборки, помеченные 0.0.3/0.0.4 — при перенумерации удалены.)
- **0.0.1** (27.09.2026): первичная сборка.

## Open
- [x] VM-тест (Win11 «Среда разработки»): установка/ярлыки/фильтр проверены; симптом «после ребута
  нет сигнала» воспроизведён (симптом) и закрыт фиксом: после ребута и входа —
  `writer ready (Local\...)`, status «frames are being written», превью показывает картинку.
  Локально установщики НЕ запускались (стоп FrameServer рвёт ktalk).
- [ ] Коммит (файлы: FrameWriter.*, VCamProducerCli.cpp, VCamVideoStreamProducer.cpp,
  vcam_installer.iss → 0.0.2, memory) — только по явному запросу пользователя.
