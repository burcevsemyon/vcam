# VCam

Виртуальная камера для Windows 11 (25H2, build 26200): in-proc COM DLL, реализующая
`IMFMediaSource`/`IMFMediaStream2`, зарегистрированная через `MFCreateVirtualCamera`.
Кадрывает отдельный процесс, пишущий в общую память.

## Состав

| Проект | Назначение |
|---|---|
| `src/MediaSource` (MediaSource.dll) | COM media source: видео-потоки 1280×720@30 и 640×480@30 (RGB32 + NV12), автоматический даунскейлинг и конверсия в потоке. `IMFMediaSourceEx`, `IKsControl`, `IMFGetService`, async worker + token queue. |
| `src/Registrar` (Registrar.exe) | Регистрация камеры: `add [name] [hold]` / `remove`. Процесс нужно держать живым (Session lifetime). |
| `src/ProducerCore` (ProducerCore.lib) | Общее ядро продюсеров: `Settings` (чтение/миграция/запись settings.json), `SettingsWatcher` (опрос 500 мс + debounce 200 мс), `FrameWriter` (запись в общую память, seqlock, FlushLast), источники `StaticImageSource` / `VideoFileSource` / `CameraSource` (захват физической камеры, MF Source Reader, letterbox), `CameraDevices` (перечисление камер), `SourceFactory`, `ToSourceConfig`. Используется хостом и CLI. |
| `src/VCamVideoStreamProducer` (VCamVideoStreamProducer.exe) | Основной продюсер-хост: tray-иконка с меню (статус, «Настройки VCam…», «Окно предпросмотра…», «Автозагрузка», «Выход»), ядро state machine (hot-switch без перезапуска, fallback NO SIGNAL при ошибках источника), мьютекс `VCamVideoStreamProducer.Instance`, автозагрузка в `HKCU\Run` по `settings.autostart`. |
| `src/VCamProducerCli` (VCamProducerCli.exe) | Консольный хост для отладки и E2E: `run [--type static\|video\|camera] [--path <file>] [--device <id>] [--settings <path>]` — то же ядро без tray (логи в stdout @30 FPS, остановка по Ctrl+C/Ctrl+Break/Esc); `list-devices` — перечисление физических камер (`id\tname` в stdout); `status` — путь/схема settings, `source.type`, секции, автозапуск, состояние хоста и writer-секции. |
| `src/ProducerTest` (ProducerTest.exe) | Пишет анимированный test pattern в общую память @30 fps. |
| `src/StaticProducer` (StaticProducer.exe) | Отдельная утилита: статическое изображение в общую память @30 fps. Понимает **legacy-поля** settings.json (`imagePath`/`mediaMode`/`mediaPath`, hot-reload ~0.7 с), аргумент командной строки — fallback. Для обычной работы используйте хост или CLI. |
| `src/VideoProducer` (VideoProducer.exe) | Отдельная утилита: видеоролик в общую память @30 fps (декод Media Foundation, letterbox 1280×720, loop). Понимает **legacy-поля** settings.json (`mediaPath`, hot-reload), `argv[1]` — fallback. Для обычной работы используйте хост или CLI. |
| `src/VCamSettingsUi` (VCamSettingsUi.exe) | C# WinForms UI: переключатель «Медиа» (статичная картинка / видеоролик / физическая камера), выбор файла, предпросмотр fit/cover, интерактивный crop (рамка мышью), просмотр 1:1 с зумом, список физических камер, запуск окна предпросмотра VCamPreview, сохранение настроек (новая схема). |
| `src/VCamPreview` (VCamPreview.exe) | Плавающее окно предпросмотра кадра: always-on-top, читает общую память, NO SIGNAL без провайдера, Esc/Ctrl+Q — выход. |
| `src/CaptureTest` (CaptureTest.exe) | Диагностический захват: `inspect`, `device [strict] [name\|index] [width] [height] [prefix]`, bare `[numFrames] [prefix]`; сохраняет BMP. |

## Сборка

```bat
"C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" VirtualCameraMediaSource.sln /p:Configuration=Release /p:Platform=x64 /m /clp:ErrorsOnly
```

Результат: `build\x64\Release\` (плюс per-project `src\*\build\`).

## Деплой и запуск (все шаги — от администратора)

1. Деплой DLL: `MediaSource.dll` → `C:\Program Files\VCam\`.
2. Регистрация камеры (процесс Registrar не закрывать!):
   ```bat
   build\x64\Release\Registrar.exe add VCam hold
   ```
3. Провайдер кадров:
   - **Основной способ — tray-хост** (отдельная консоль; в трее меню с настройками,
     предпросмотром и автозагрузкой):
     ```bat
     build\x64\Release\VCamVideoStreamProducer.exe
     ```
     Источник и путь берёт из `%APPDATA%\VCam\settings.json`; смена `source.type`
     и/или путей подхватывается на лету (hot-switch, без перезапуска).
   - **Консольный хост (отладка/E2E)** — то же ядро без tray, логи в stdout:
      ```bat
      build\x64\Release\VCamProducerCli.exe run
      build\x64\Release\VCamProducerCli.exe run --type video --path C:\path\clip.mp4
      build\x64\Release\VCamProducerCli.exe run --type camera
      build\x64\Release\VCamProducerCli.exe list-devices
      build\x64\Release\VCamProducerCli.exe status
      ```
      `--type`/`--path`/`--device` фиксируют параметр на весь запуск (правки в
      settings.json для него игнорируются); без них всё читается из файла live.
      Остановка —
     Ctrl+C / Ctrl+Break / Esc. Если tray-хост уже запущен — `run` предупреждает
     в stderr и продолжает (два писателя в одну секцию допустимы только для отладки).
   - Окно предпросмотра (отдельное окно поверх всех, читает общую память):
     ```bat
     build\x64\Release\VCamPreview.exe
     ```
     Esc — выход; ту же кнопку имеет UI и меню хоста (режим «видеоролик»).
   - Отдельные утилиты (legacy-чтение settings, без hot-switch по `source.type`):
     `ProducerTest.exe` (test pattern), `StaticProducer.exe [image]`,
     `VideoProducer.exe [clip]`.
4. E2E тестирование:
   ```powershell
   powershell -ExecutionPolicy Bypass -File e2e_test.ps1
   ```
   Собирает решение, регистрирует камеру, запускает `VCamProducerCli.exe run`
   (фаза A — с overrides `--type video --path`, фаза B — hot-switch static→video
   через перезапись settings.json, фаза C — физическая камера: `list-devices`,
   `run --type camera --device <id>`, негатив с несуществующим id → NO SIGNAL;
   без камер в системе фаза C помечается SKIP), проверяет кадры `CaptureTest`
   (движение / статика) и логи CLI. `settings.json` сохраняется в бэкап и
   восстанавливается байт-в-байт в конце. Exit code 0 = успех.
5. Проверка: камера видна в «Параметры → Bluetooth и устройства → Камеры» и в любых приложениях;
   без запущенного продюсера — чёрные кадры (fallback, штатное состояние).
   Диагностика:
   ```bat
   build\x64\Release\CaptureTest.exe inspect
   build\x64\Release\CaptureTest.exe device 1 1280 720 C:\path\to\prefix
   build\x64\Release\CaptureTest.exe device 1 640 480 C:\path\to\prefix
   ```

Важно:

- Камера имеет **Session lifetime**: как только процесс `Registrar` завершён, камера исчезает из PnP.
- FRIENDLY_NAME устройства в перечислении может быть `(none)` — в `CaptureTest` device mode
  выбирать нашу камеру по **индексу** (`device 1`), а не по имени.
- `register.bat` / `unregister.bat` — быстрые сценарии регистрации/снятия.

## Настройки (settings.json)

Файл `%APPDATA%\VCam\settings.json`: пишет UI (и `e2e_test.ps1`), читают хост и CLI
(опрос каждые 500 мс, debounce 200 мс). Старый плоский формат (`imagePath`/`mediaMode`/
`mediaPath`/…) принимается при чтении и мигрируется на лету — сам файл не
переписывается; сохраняется всегда новая схема:

```json
{
  "source": { "type": "static" },
  "static": {
    "path": "C:\\path\\to\\image.jfif",
    "scaleMode": "fit",
    "cropX": 0,
    "cropY": 0,
    "cropW": 0,
    "cropH": 0,
    "cropKeepAspect": false
  },
  "video": { "path": "C:\\path\\to\\clip.mp4" },
  "camera": { "id": "\\\\?\\usb#vid_046d&pid_0949#...\\global", "name": "Brio 90" },
  "autostart": true
}
```

| Поле | Значения | Смысл |
|---|---|---|
| `source.type` | `static` \| `video` \| `camera` | какой источник кормит камеру. Иной токен сохраняется как есть — читатель уйдёт в fallback (NO SIGNAL) |
| `static.path` | путь к файлу | источник картинки (PNG/JPG/BMP/JFIF/…) |
| `static.scaleMode` | `fit` \| `cover` \| `crop` | `fit` — вписать в 1280×720 с чёрными полосами; `cover` — заполнить, center-crop без искажений; `crop` — обрезать по прямоугольнику ниже. Только для картинки: у видео всегда letterbox |
| `static.cropX/Y/W/H` | пиксели исходника | область обрезки (только для `crop`); невалидный прямоугольник → clamp к границам, нулевой → вся картинка. По умолчанию результат **растягивается на 1280×720 без сохранения пропорций** |
| `static.cropKeepAspect` | `true` \| `false` | только для `crop`: `true` — вписать область с сохранением пропорций (чёрные полосы) вместо растяжки |
| `video.path` | путь к файлу | источник ролика (MP4/MKV/…), letterbox 1280×720, бесконечный loop |
| `camera.id` | MF symbolic link | физическая камера (USB-устройство); пусто **и** пустое `camera.name` → источник не открывается, NO SIGNAL до выбора в UI |
| `camera.name` | friendly name | запасной ключ поиска (точное имя → подстрока), если `id` не совпал (камера переставлена в другой порт); обычно заполняет UI |
| `autostart` | `true` \| `false` | автозагрузка tray-хоста: при старте хост применяет флаг к `HKCU\Run\VCamAutostart`; пункт меню «Автозагрузка» переключает и сохраняет. CLI `autостart` только показывает в `status` |

- Сценарий работы: UI сохраняет файл → хост/CLI опрашивает его каждые 500 мс
  (debounce 200 мс) и переключает тип/путь **без перезапуска** (hot-switch: пока
  новый источник не открыт, пишется последний кадр старого — `FlushLast`; если
  открыть не удалось за 5 с — fallback, камера держит последний кадр/чёрный).
- Ошибка загрузки (удалённый файл) — лог в консоль, трансляция продолжается
  со старым кадром; источник перепроверяется каждую секунду.
- Отдельные утилиты `StaticProducer`/`VideoProducer` читают **только legacy-поля**
  (`imagePath`/`mediaMode`/`mediaPath`) — новую схему они не понимают.
- CLI overrides: `--type`/`--path`/`--device` фиксируют параметр на весь запуск
  (правки settings.json для него игнорируются), `scaleMode`/`crop*` всегда из
  файла, `--settings <path>` — альтернативный путь к файлу; `--device` допустим
  только с `--type camera`.

UI (`src\VCamSettingsUi`): комбо **«Медиа»** — «статичная картинка» / «видеоролик» /
«физическая камера»;
фильтр диалога выбора файла зависит от режима (изображения / видео). Для картинки —
предпросмотр fit/cover (та же математика, что в C++), режим `crop` — интерактивная
рамка мышью прямо на предпросмотре (перетаскивание центра = сдвиг, ручки по
углам/краям = размер, перетаскивание вне рамки = новая область; поля X/Y/Ш/В
синхронны в обе стороны, чекбокс «Сохранять пропорции» — letterbox вместо
растяжки), «Просмотр полный» (оригинал, колесо — зум 25–400 %). Для видео fit/cover/crop
и crop-поля скрыты (масштаб не применяется — всегда letterbox 1280×720), вместо
превью — инфо-панель: путь к ролику и кнопка **«Открыть окно предпросмотра
(VCamPreview)»** (`VCamPreview.exe` ищется рядом с `VCamSettingsUi.exe` и в
`build\x64\Release\`; не найден — кнопка отключена с подсказкой). Кнопка
«Сохранить настройки» пишет новую схему (`source`/`static`/`video`/`camera`/`autostart`)
по текущему режиму UI; в видео-режиме `static`-секция и `autostart` остаются как
были на диске. Окно предпросмотра запускается отдельным процессом (не дочерним).

В режиме «физическая камера» UI показывает панель со списком устройств: список
запрашивается у `VCamProducerCli.exe list-devices` (ищется рядом с `VCamSettingsUi.exe`
и в `build\x64\Release\`; не найден — список отключён с подсказкой), есть кнопка
«Обновить список». Выбор сохраняется в секцию `camera` (`id` + `name`); сохранённое
устройство, пропавшее из списка, показывается пометкой «(недоступно)», но его `id`
не теряется. Сохранение режима camera без выбранного устройства допустимо — хост
даст NO SIGNAL, пока камера не будет выбрана.

## Физическая камера (camera)

Третий тип источника (`source.type = "camera"`) транслирует в виртуальную камеру
поток с веб-камеры через Media Foundation:

- **Устройство** выбирается по `camera.id` (MF symbolic link, стабилен для
  USB-порта); если не совпало — по `camera.name` (точное имя, затем подстрока).
  Оба поля пусты → Open → false → NO SIGNAL (until выбора в UI).
- **Формат**: запрашивается RGB32 1280×720@30; если камера не даёт такой выход —
  берётся ближайший тип и кадр честно вписывается (letterbox fit, без искажений,
  чёрные полосы); ровно 1280×720 — копия без масштабирования. Захват идёт в
  фоновом потоке, `Render` отдаёт последний кадр и никогда не блокируется на MF.
- **Отсутствие/занятость/отключение камеры** — штатный путь: `Open`/`Render` →
  false → хост уходит в fallback NO SIGNAL и перепроверяет устройство каждую
  секунду (камера «приехала» — поток восстанавливается сам).
- **Hot-switch**: переключение static↔video↔camera без перезапуска, пока новый
  источник не дал первый кадр, пишется последний удачный кадр (`FlushLast`).
- **CLI**: `list-devices` (печатает `id\tname`), `run --type camera [--device <id>]`.
- Практика: список камер UI и `list-devices` показывают камеры системы; если
  устройство занято другим приложением — фиксируйте факт, `Open` вернёт ошибку
  и вы получите NO SIGNAL (это штатное поведение, не баг).

## Контракт общей памяти

| Поле | Значение |
|---|---|
| Section | `Global\VCam.FrameBuffer.v1` |
| Ready event | `Global\VCam.FrameReady.v1` |
| Разрешение | 1280×720, stride 5120, RGB32 (BGRX) |
| Размер кадра | `VCamFrameSize = VCamHeight × VCamStride = 3 686 400` байт |
| Слоты | 8 |
| Seqlock | нечётный `seq` = идёт запись, чётный = стабилен |
| Таймаут ready event | 40 мс → fallback (последний закэшированный кадр / чёрный) |
| DACL | SY, BA, LS, NS, WD (полный доступ) |

Consumer ждёт ready event (40 мс), читает `frameWriteIndex` под seqlock и копирует кадр.

## Ключевой баг (найдено и исправлено 13.09.2026)

`VCamFrameSize` был вычислен как `VCamWidth × VCamStride` (6 553 600) вместо
`VCamHeight × VCamStride` (3 686 400): `memset` в `FallbackFrame` переполнял sample buffer
FrameServer на 2,86 МБ → AV 0xC0000005 → краш FrameServer (симптом: «кадр мелькнул один раз,
затем ошибка в настройках камеры»; в consumer — `0xC00D3E9B`).
Доказано по minidump + cdb (символы: `srv*C:\Symbols*https://msdl.microsoft.com/download/symbols`),
фикс — одна строка в `src/Common/SharedMemoryContract.h`. После фикса: 20/20 кадров
(без провайдера — чёрные, с провайдером — pattern), крашей FrameServer нет.

## Файлы

```
src/Common/
  GUIDs.h                        CLSID_VCamMediaSource {B2B674D4-9CF0-461C-BDCE-3D56FBB41356}
  SharedMemoryContract.h         layout section, имена, DACL, VCamFrameSize
  SharedMemoryFrameSource.h/.cpp consumer: ожидание события, копия кадра (seqlock), fallback
  SampleAllocatorControl.h       IKS_SAMPLEALLOCATORCONTROL
  ProducerApi.h                  SourceConfig, IFrameSource (общий API продюсеров)
src/ProducerCore/
  Settings.h/.cpp                чтение/миграция/запись settings.json, ToSourceConfig
  SettingsWatcher.h/.cpp         опрос 500 мс + debounce 200 мс, событие dirty
  FrameWriter.h/.cpp             запись кадра в секцию (seqlock, FlushLast, Close)
  VideoFileSource.h/.cpp         MP4/MKV -> RGB32 letterbox (MF Source Reader, loop)
  CameraSource.h/.cpp            физическая камера -> RGB32 letterbox (фоновый захват)
  CameraDevices.h/.cpp           перечисление камер (id + friendly name)
  StaticImageSource.h/.cpp       PNG/JPG/BMP -> RGB32 (WIC, fit/cover/crop)
  SourceFactory.h/.cpp           тип -> источник
src/MediaSource/
  MediaSource.h/.cpp             IMFMediaSource(+Ex) + IKsControl + IMFGetService
  MediaStream.h/.cpp             IMFMediaStream2: async worker, token queue, RGB32→NV12
  Activator.h/.cpp               активация (classless object)
  dllmain.cpp                    ATL COM factory, DllRegisterServer
  MediaSource.def                экспорты
src/Registrar/main.cpp           MFCreateVirtualCamera, add/hold/remove
src/VCamVideoStreamProducer/     tray-хост: ядро state machine, меню, автозапуск, мьютекс
src/VCamProducerCli/             консольный хост: run/status/list-devices, Ctrl+C/Esc, логи в stdout
src/ProducerTest/main.cpp        test pattern → общая память
src/StaticProducer/StaticProducer.cpp legacy-утилита: статическое изображение (WIC, fit/cover/crop)
src/VideoProducer/VideoProducer.cpp   legacy-утилита: видеоролик (MF Source Reader, letterbox, loop)
src/VCamPreview/VCamPreview.cpp     окно предпросмотра из общей памяти (always-on-top, NO SIGNAL, Esc)
src/VCamSettingsUi/               C# WinForms UI: медиа static|video|camera, выбор/предпросмотр/зум/crop-рамка, список камер, запуск VCamPreview, settings.json (новая схема)
src/CaptureTest/main.cpp         inspect/capture → BMP
register.bat, unregister.bat     регистрация (от администратора)
e2e_test.ps1                     автоматический E2E-тест (через VCamProducerCli run)
memory.md                        состояние проекта (resume-документ)
```
