# vcam-producer (parent memory) — VCamVideoStreamProducer: абстрактный продьюсер

## Задача
Динамически переключаемый провайдер кадров вместо ручного выбора exe.
Утверждённые пользователем решения (26.09.2026):
1. **Формат настроек — секции по типам**: `source.type` (единственное переключаемое поле) + секции `static`/`video` (у каждого свой `path` и параметры) + `autostart` в корне. Миграция: старый формат (корневые `imagePath/mediaMode/mediaPath/scaleMode/cropX/Y/W/H/cropKeepAspect`) читается и раскладывается по секциям; писать — только новый.
   ```json
   { "source": {"type": "static"},
     "static": {"path": "...", "scaleMode": "crop", "cropX":128, "cropY":199, "cropW":660, "cropH":971, "cropKeepAspect": true},
     "video":  {"path": "..."},
     "autostart": true }
   ```
2. **crop/keepAspect — только в секции static** (видео не режется, letterbox; единый конвейер = один writer/контракт, параметры рендера из секции активного источника).
3. **Отдельный `ProducerCore.lib`** (статик-лист) — ядро.
4. **Старые exe** (`StaticProducer`, `VideoProducer`) — пока оставить отладочными, не удалять, не изменять.
5. **Хост `VCamVideoStreamProducer.exe`**: Win32 GUI с иконкой в трее (меню: статус источника, открыть настройки, открыть предпросмотр, автозагрузка ✓/✗, выход); автозапуск = HKCU Run `VCamAutostart`, по умолчанию ВКЛ, хранится в `autostart`; запуск хоста из UI (кнопка «Запустить/Остановить» + индикатор статуса).
6. **CLI `VCamProducerCli.exe`** — отдельная консольная утилита для отладки и e2e: `run [--type static|video] [--path Y]` (хост без трея, stdout-логи, Esc/Ctrl+C), `status`.
7. **Бесшовное переключение**: при смене `source.type` писатель продолжает класть последний удачный кадр, пока новый источник не даст первый `Render=true` → в превью без провала NO SIGNAL (~0.7 с горячего переключения). **Fallback при ошибке**: если источник не открылся/`Render=false` — НЕ писать → камера FallbackFrame (чёрные кадры), превью NO SIGNAL. НЕ делать «keep playing previous clip» (текущее поведение VideoProducer).
8. Новый источник = класс + секция + строка в фабрике.

## Интерфейс (согласован)
```cpp
// src/Common/ProducerApi.h
struct IFrameSource {
    virtual bool Open(const SourceConfig&, std::wstring& err) = 0;
    virtual bool Render(uint8_t* bgrx, int stride, std::wstring& err) = 0; // 1280x720, false = нет данных
    virtual void Close() = 0;
    virtual const wchar_t* Name() const = 0;
};
```

## Ключевые факты (для subagent'ов)
- Репо: `C:\Users\Semen\source\repos\VCam`, **ветка `feature/vcam-video-stream-producer`** (main = `283329e`).
- Решение: `VirtualCameraMediaSource.sln` (Release|x64); MSBuild: `"C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" VirtualCameraMediaSource.sln /p:Configuration=Release /p:Platform=x64 /m /v:m /nologo`; csproj — сначала `dotnet restore`.
- Контракт памяти: `src/Common/SharedMemoryContract.h` — `Global\VCam.FrameBuffer.v1`, seqlock, 1280×720 BGRX stride 5120, `VCamFrameSize=3686400`, 8 слотов, DACL `vcam::VCamDacSddl`. НЕ менять.
- Референсы для переноса: `src/StaticProducer/StaticProducer.cpp` (writer-каркас: CreateFileMapping+хедер, 30 FPS, slot=(idx+1)%8 под CS, settings-poll 500ms, Escape, stdout+fflush; `LoadAndScaleImage` fit/cover/crop/keepAspect, `EffectiveImagePath`), `src/VideoProducer/VideoProducer.cpp` (MF SourceReader, loop SetPosition(0), letterbox HighQualityBicubic, hot-reload mediaPath; путь MF: `MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING=TRUE`, `SetCurrentMediaType(RGB32)`).
- Settings: `%APPDATA%\VCam\settings.json`. Формат см. выше. C++ — ручной плоский парсер (`JsonGetString/GetInt/GetBool` в Common или своё); C# UI — `System.Text.Json`; hot-reload 500 мс; `argv[1]` в старых exe переопределяет путь (не трогать).
- UI: `src/VCamSettingsUi/` — MainForm.cs (переключатель static|video, static-панель с crop, video-панель + кнопка «Открыть окно предпросмотра» → `<repo>\build\x64\Release\VCamPreview.exe`), Settings.cs (тест-seam `filePath` в Load/Save), CropPreviewControl.cs. Сейчас пишет старую схему — нужно перевести на новую (секции), сохранить back-compat чтение и round-trip тесты.
- E2E: `e2e_test.ps1` (последний прогон exit 0); CaptureTest: `CaptureTest.exe inspect` (count=2 = ок), `CaptureTest.exe 2 <prefix>` → BMP 1280×720 24-bit (2 764 854 B); пиксельные проверки — `[System.Drawing.Bitmap]::FromFile` + GetPixel (PS5.1: `[IO.File]::WriteAllText(..., UTF8Encoding($false))` — BOM-ловушка).
- Тестовые ролики: `e2e_output\test_video.mp4`, `test_video2.mp4`, `pattern_top_white.mp4`.
- Камера зарегистрирована (Registrar.exe hold, elevated); процессы НЕ убивать без спроса: Registrar (29748), текущий VideoProducer (26916) — НО: при готовности хоста пользователь переключится; старые exe не трогаем, UI/превью пользователя не убивать.
- Питфолы: MSB3027 (закрыть свой exe перед сборкой); GDI+ только HighQualityBicubic; не убивать чужие процессы; UI-процессы пользователя не киллить.
- Текущее settings.json пользователя — СТАРЫЙ формат (static + jfif-путь), не перезаписывать до готовности миграции (миграция обязана читать старый формат!).

## План (последовательно, лимит 2 раунда на субагента)
- [x] **Subagent 1 — ProducerCore.lib** (DONE, 2 раунда): см. `vcam-producer-core.memory.md`. API: SourceConfig/IFrameSource/CreateSource, Settings (Load-миграция/Save-новая/Serialize), FrameWriter (WriteFrame+FlushLast, кэш последнего кадра), SettingsWatcher. Сборка exit 0, верификация FAILS=0.
- [x] **Subagent 2 — VCamVideoStreamProducer (tray+autostart) + UI** (DONE_WITH_GAPS, см. `vcam-producer-tray.memory.md`): мьютекс `VCamVideoStreamProducer.Instance` + Stop-event `VCamVideoStreamProducer.Stop`, автозапуск HKCU Run\VCamAutostart (источник правды = settings.autostart), меню трей (статус/настройки/превью/автозагрузка/выход), UI: кнопка+статус хоста, Settings.cs на новой схеме (25/25 тестов). Бесшовность maxGap 92 мс, fallback NO SIGNAL — проверено. Gap: скриншот меню трей не снят (foreground-политика Win).
- [x] **Subagent 3 — VCamProducerCli.exe + e2e + README** (DONE, см. `vcam-producer-cli.memory.md`): `run [--type/--path/--settings]` (Ctrl+C → exit 0), `status`; e2e_test.ps1 переписан на CLI (backup→фаза A→фаза B hot-switch→restore SHA256) = exit 0; README обновлён. **Важный фикс**: баг `ProducerCore/Settings.cpp` — FindObjectRange матчил `"video"` по значению (`"type":"video"`) → video.path читался из секции static; фикс FindKeyPos (после ключа обязатен `:`), задевает и tray-хост.
- [x] **Контрольный smoke (основной, после sub3)**: хост 5816 + новая схема: type=video → движущиеся кадры (nonBlack=690/900, differ=True); type=static → identical (nonBlack=342/900). settings восстановлен байт-в-байт (legacy). Стек поднят: хост 5816, VCamPreview 23728, UI 28864.
- [x] **Subagent 4 — Preview: GDI+ масштабирование (муар) + single-instance Preview/UI** (DONE, 1 раунд, см. `vcam-producer-previewfix.memory.md`): PaintFrame через GDI+ HighQualityBicubic (MAE 0.023 к эталону, vs nearest 7.174), GDI-утечек нет, ~11 мс/кадр (качество > скорость); мьютексы `VCamPreview.Instance` / `VCamSettingsUi.Instance` (второй запуск exit 0, 1 процесс/окно, разворачивает существующее); UI round-trip 25/25. **Питфолл**: PS P/Invoke `FindWindowW(cls,$null)` → marshals "" (не NULL) — в C++ nullptr работает.
- [x] Ручная проверка пользователем — «работает, изображение исправлено».
- [x] **Дефект №3 — мигание Preview после остановки хоста** (фикс в основном чате): причина — `Disconnect()` обнулял `lastSeqChange`, `Connect()` ставил его заново → старый кадр снова «свежий» → цикл статика→NO SIGNAL каждые ~3 с. Фикс: не обнулять lastSeq/lastSeqChange в Disconnect, обновлять lastSeqChange при reconnect только если seq реально изменился, реконнект по `nextReconnectAt` (rate-limit). Живая проверка: 48.7 → stop → 1.5 с (timeout) → 15.5 стабильно 11 с (реконнекты не мигают) → рестарт хоста → 48.7.
- [x] **Дефект №4 — кракозябры кириллицы в заголовке Preview** (UTF-8 без BOM, MSVC без /utf-8 читал CP1251): добавлен Directory.Build.targets (`/utf-8` для всех vcxproj), локальные дубли из 2 vcxproj убраны; проверено: заголовок «VCam Preview — нет сигнала» читается, статика без регрессии; после рестарта хоста статика возвращается ≤~4 с (тайминг цикла реконнекта, не баг). Сборка exit 0. Коммит 2.
- [x] Коммит — `1420d46` (4 файла, +192/−15), дерево чистое.
- [x] **Источник «физическая камера» (`L"camera"`)** — см. `vcam-camera.memory.md` (+ её
  субагенты `vcam-camera-{core,cli,ui}.memory.md`): `CameraSource`/`CameraDevices` в
  ProducerCore (MF, фоновый захват, letterbox fit, Render=false до первого кадра),
  секция settings `camera {id,name}` (пустая → NO SIGNAL до выбора), SourceConfig.camName,
  CLI `list-devices` + `--device`, e2e фаза C (SKIP без камеры), UI: 3-й режим + панель
  устройств (тесты 43/43), README. Сборка exit 0, e2e exit 0, живая проверка: поток в
  Preview (mean 76.5), hot-switch static↔camera без чёрного (10/10 BMP non-black),
  NO SIGNAL при пустой секции/битом id. Ветка `feature/vcam-camera-source`, коммитов нет.

## Итог/риски
- Старые exe (StaticProducer/VideoProducer) не тронуты (legacy/отладочные).
- HKCU Run\VCamAutostart записан (штатное поведение хоста, default вкл).
- Проверить вручную: меню трей (визуально), Esc в CLI, UI-сохранение в новую схему → хост подхватывает.

## Память субагентов
- Промпт субагента += этот файл (память вышестоящей) + свой `<task>.memory.md` (писать статус/факты/риски).
