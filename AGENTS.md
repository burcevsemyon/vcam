# VCam — виртуальная камера Windows

Виртуальная камера (Media Foundation): in-proc COM DLL + shared-memory
пайплайн продьюсеров кадров. Весь код — в `VirtualCameraMediaSource/`
(там же `.sln`, `README.md`, память задач `*.memory.md`).

## Структура (`VirtualCameraMediaSource/src/`)

| Каталог | Что |
|---|---|
| `MediaSource/` | Камера (in-proc COM DLL). Регистрация: `build\x64\Release\Registrar.exe add VCam hold` (elevated, процесс не закрывать); страницу «Камеры» Windows Settings перезапускать. |
| `Common/` | Контракты: `SharedMemoryContract.h` (секция `Global\VCam.FrameBuffer.v1`, 1280×720 BGRX, stride 5120, 8 слотов seqlock — **не менять без согласования**), `ProducerApi.h` (`IFrameSource`/`SourceConfig`/`CreateSource`), `SharedMemoryFrameSource` (чтение камеры). |
| `ProducerCore/` | Библиотека источников: `StaticImageSource`, `VideoFileSource`, `FrameWriter` (единственный писатель, 30 FPS pacing, `FlushLast` для hot-switch), `Settings` (новая схема + миграция legacy), `SettingsWatcher` (hot-reload), `SourceFactory`. |
| `VCamVideoStreamProducer/` | Основной tray-хост: single-instance (`VCamVideoStreamProducer.Instance`), Stop-event, автозапуск через задачу Task Scheduler `VCamHost` по `settings.autostart`, hot-switch static↔video, fallback NO SIGNAL. |
| `VCamProducerCli/` | Консольный хост отладки/e2e: `run [--type --path --settings]`, `status`. |
| `VCamPreview/` | Плавающее окно предпросмотра (GDI+ HighQualityBicubic, single-instance `VCamPreview.Instance`). |
| `VCamSettingsUi/` | C# WinForms настройки (выбор источника, crop, превью, кнопки запуска хоста; single-instance `VCamSettingsUi.Instance`). |
| `CaptureTest/` | Диагностика: захват кадров камеры в BMP (`inspect`, `device`, direct `<n> <prefix>`). |
| `ProducerTest/` | Анимированный test pattern → общая память. |
| `StaticProducer/`, `VideoProducer/` | Legacy-утилиты (оставлены для отладки; для работы — хост или CLI). |

## Сборка

```bat
"C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" VirtualCameraMediaSource.sln /p:Configuration=Release /p:Platform=x64 /m /v:m /nologo
```

- exit 0 = успех; csproj (`VCamSettingsUi`) — сначала `dotnet restore`.
- MSB3027 (locked exe) — закрыть запущенные exe проекта перед сборкой.
- `/utf-8` для всех C++ задано в `Directory.Build.targets` (кириллица в
  wide-литералах безопасна; без него MSVC читал UTF-8 как CP1251 → кракозябры).

## Тесты

- **E2E**: `powershell -ExecutionPolicy Bypass -File e2e_test.ps1` (из
  `VirtualCameraMediaSource/`), exit 0 = SUCCESS. Перед прогоном остановить
  tray-хост — два писателя интерливят кадры. Вся эмпирика (фазы A/B, ассерты,
  питфолы, ручная диагностика): skill **vcam-e2e**
  (`.agents/skills/vcam-e2e/SKILL.md`) — читать его, а не пересматривать скрипт.
- Постоянного unit-тест-проекта нет; UI round-trip тесты создаются временным
  проектом в `%TEMP%` (см. `vcam-video-ui.memory.md`).

## Настройки и среда

- `%APPDATA%\VCam\settings.json` — новая схема
  `{"source":{"type":"static|video"},"static":{...},"video":{...},"autostart":bool}`;
  пишет UI и e2e, читают хост и CLI; **UTF-8 без BOM**
  (`Set-Content -Encoding UTF8` даёт BOM — не использовать); файл не блокируется,
  изменения подхватываются на лету.
- Камера: 1280×720@30, fallback при отсутствии провайдера — кэш последнего
  кадра, без кэша — NO SIGNAL.

## Git и память

- Ветка `main`; memory-файлы `*.memory.md` и `memory.md` (в `VirtualCameraMediaSource/`) —
  **локальные, в git НЕ идут** (в `.gitignore`: `memory.md`, `*.memory.md`); в репо — только
  код и артефакты.
- Перед доработкой читать память релевантной задачи
  (`vcam-producer*.memory.md`, `vcam-video*.memory.md`) — там факты, фиксы и
  питфолы; свой статус писать в `<task>.memory.md`.

## Конвенции

- Ответы по-русски; код/SQL/deploy/коммит — только после явного разрешения
  (см. глобальный `AGENTS.md`).
- Изолируемую работу отдавать субагентам (лимит 2 раунда) с памятью
  вышестоящей задачи в промпте.
