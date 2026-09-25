# VCam

Виртуальная камера для Windows 11 (25H2, build 26200): in-proc COM DLL, реализующая
`IMFMediaSource`/`IMFMediaStream2`, зарегистрированная через `MFCreateVirtualCamera`.
Кадрывает отдельный процесс, пишущий в общую память.

## Состав

| Проект | Назначение |
|---|---|
| `src/MediaSource` (MediaSource.dll) | COM media source: видео-потоки 1280×720@30 и 640×480@30 (RGB32 + NV12), автоматический даунскейлинг и конверсия в потоке. `IMFMediaSourceEx`, `IKsControl`, `IMFGetService`, async worker + token queue. |
| `src/Registrar` (Registrar.exe) | Регистрация камеры: `add [name] [hold]` / `remove`. Процесс нужно держать живым (Session lifetime). |
| `src/ProducerTest` (ProducerTest.exe) | Пишет анимированный test pattern в общую память @30 fps. |
| `src/StaticProducer` (StaticProducer.exe) | Транслирует статическое изображение (PNG/JPG/BMP/…) в общую память @30 fps. Путь и режим масштабирования берёт из `%APPDATA%\VCam\settings.json` (hot-reload ~0.7 с), аргумент командной строки — fallback. |
| `src/VCamSettingsUi` (VCamSettingsUi.dll) | C# WinForms UI: выбор картинки, предпросмотр fit/cover, просмотр 1:1 с зумом, сохранение настроек. |
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
3. Провайдер кадров (отдельная консоль):
   - Анимированный тестовый паттерн:
     ```bat
     build\x64\Release\ProducerTest.exe
     ```
   - Статическое изображение (настройки или файл):
     ```bat
     build\x64\Release\StaticProducer.exe path\to\image.png
     ```
     без аргумента — путь из `%APPDATA%\VCam\settings.json` (создаёт UI).
4. E2E тестирование:
   ```powershell
   powershell -ExecutionPolicy Bypass -File e2e_test.ps1
   ```
4. Проверка: камера видна в «Параметры → Bluetooth и устройства → Камеры» и в любых приложениях;
   без ProducerTest — чёрные кадры (fallback, штатное состояние).
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

## Настройки StaticProducer

Файл `%APPDATA%\VCam\settings.json`:

```json
{
  "imagePath": "C:\\path\\to\\image.jfif",
  "scaleMode": "fit"
}
```

| Поле | Значения | Смысл |
|---|---|---|
| `imagePath` | путь к файлу | источник картинки (PNG/JPG/BMP/JFIF/…) |
| `scaleMode` | `fit` \| `cover` | `fit` — вписать в 1280×720 с чёрными полосами; `cover` — заполнить, center-crop без искажений |

- Сценарий работы: UI сохраняет файл → `StaticProducer` опрашивает его каждые 500 мс
  (debounce 200 мс) и перезагружает картинку/режим **без перезапуска** (~0.7 с).
- Приоритет: `argv[1]` переопределяет `imagePath` при запуске, но `scaleMode`
  всегда берётся из файла (его нет — `fit`).
- Ошибка загрузки (удалённый файл) — лог в консоль, трансляция продолжается со старым кадром.

UI (`src\VCamSettingsUi`): выбор файла → предпросмотр fit/cover (та же математика, что
в C++), «Просмотр полный» (оригинал, колесо — зум 25–400 %), «Сохранить настройки».

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
src/MediaSource/
  MediaSource.h/.cpp             IMFMediaSource(+Ex) + IKsControl + IMFGetService
  MediaStream.h/.cpp             IMFMediaStream2: async worker, token queue, RGB32→NV12
  Activator.h/.cpp               активация (classless object)
  dllmain.cpp                    ATL COM factory, DllRegisterServer
  MediaSource.def                экспорты
src/Registrar/main.cpp           MFCreateVirtualCamera, add/hold/remove
src/ProducerTest/main.cpp        test pattern → общая память
src/StaticProducer/StaticProducer.cpp статическое изображение (WIC, fit/cover) → общая память, settings.json hot-reload
src/VCamSettingsUi/               C# WinForms UI: выбор/предпросмотр/зум, settings.json
src/CaptureTest/main.cpp         inspect/capture → BMP
register.bat, unregister.bat     регистрация (от администратора)
e2e_test.ps1                     автоматический E2E-тест
memory.md                        состояние проекта (resume-документ)
```
