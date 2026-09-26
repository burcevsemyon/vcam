# vcam-producer-core (subagent 1) — ProducerCore.lib — DONE

Задача: этап 1/3 — статическая библиотека ProducerCore.lib (ядро абстрактного продьюсера).
Ветка: feature/vcam-video-stream-producer. Память вышестоящей: vcam-producer.memory.md (+ vcam-video.memory.md).
Лимит 2 раунда: раунд 1 = 2 ошибки компиляции (C2110 в Serialize, const CS в SettingsWatcher::Current) → правки; раунд 2 = сборка решения exit 0.

## Статус
- [x] Все шаги выполнены. Сборка решения: exit 0, ProducerCore.lib -> build\x64\Release\ProducerCore.lib.

## Факты (для вышестоящего/следующих субагентов)
- Создано (только новые файлы + .sln):
  - `src/Common/ProducerApi.h` — SourceConfig (type/path/scaleMode/cropX/Y/W/H/cropKeepAspect, operator==), IFrameSource (Open/Render/Close/Name), `std::unique_ptr<IFrameSource> CreateSource(const std::wstring& type)` (L"static"→StaticImageSource, L"video"→VideoFileSource, иное→nullptr).
  - `src/ProducerCore/Settings.h/.cpp` — Settings{sourceType, st(StaticSection), video(VideoSection), autostart}, Load(path) (новая схема; без ключей source/static/video → миграция из старой), Save(path) (ТОЛЬКО новая схема, UTF-8 без BOM), Serialize(), operator==; DefaultSettingsPath()=%APPDATA%\VCam\settings.json; ToSourceConfig(s[, type]). Ручной плоский JSON-парсер + FindObjectRange (brace-matching со строками) для секций.
  - `src/ProducerCore/StaticImageSource.h/.cpp` — WIC LoadAndScaleImage (перенос из StaticProducer, HighQualityCubic), Open грузит в буфер 1280x720 BGRX; повторный Open с тем же конфигом = no-op; Render = memcpy с учётом stride.
  - `src/ProducerCore/VideoFileSource.h/.cpp` — MF SourceReader в фоновом потоке (ENABLE_VIDEO_PROCESSING-цепочка кандидатов RGB32, frame-holding по ts, луп SetPosition(0), letterbox bilinear — как в оригинале VideoProducer); Render отдаёт кэш под CS; ошибка открытия/декодирования → failed_ + Render=false с причиной (никакого keep-playing-previous); Open повторно при failed_/смене конфига перезапускает поток.
  - `src/ProducerCore/FrameWriter.h/.cpp` — Open (CreateFileMapping Global\VCam.FrameBuffer.v1 + DACL SDDL + ready-event + хедер), WriteFrame(bgrx,stride) (кэш 3 686 400 B последнего удачного кадра + seqlock-публикация slot=(idx+1)%8 под CS + 30 FPS pacing Sleep по QPC), FlushLast() (повторная публикация кэша для hot-switch), Close/деструктор; ошибки → OutputDebugString + false.
  - `src/ProducerCore/SettingsWatcher.h/.cpp` — Start(path, cb)/Stop/Current(Settings&)/HasCurrent; поток: poll 500ms + сравнение сырого UTF-8 содержимого + debounce 200ms + operator== → cb(const Settings&) из потока (try/catch); пропавший файл сбрасывает базу.
  - `src/ProducerCore/SourceFactory.cpp` — реализация CreateSource.
  - `src/ProducerCore/ProducerCore.vcxproj` — StaticLibrary, Release|x64, GUID {4259BECC-52DD-4B64-8ACC-B93D93DDA175}, toolset v145, stdcpp20, /MT, NDEBUG;_LIB;_WIN32_WINNT=0x0A00, IncludeDirectories $(ProjectDir);$(ProjectDir)..\Common, OutDir/IntDir как у соседей. В .sln добавлен проект + 2 строки Release|x64 (diff = +4 строки, LF сохранён).
- Сборка: MSBuild решения Release|x64 /m → **exit 0** (без MSB3027/MSB3277; старые exe пересобраны, не тронуты). 2 раунда из 2.
- Верификация (во временной папке, не в репо): миграция реального settings.json пользователя → OK + round-trip Save/Load equal=1; sources_test (связка с ProducerCore.lib): static.Open/Render(ненулевой буфер)/reopen/bad-path, video.FirstFrame(test_video.mp4), video bad-file → Render=false (0x80070003) — FAILS=0. Ни один тест не трогал shared memory/чужие процессы; реальный settings.json только читался.
- git status: M .sln, ?? ProducerApi.h, ?? ProducerCore/, ?? vcam-producer-core.memory.md, ?? vcam-producer.memory.md (родительский, не трогал).

## Вывод миграции (фактический, из реального файла)
```json
{
  "source": { "type": "static" },
  "static": { "path": "C:\\Users\\Semen\\Downloads\\Gemini_Generated_Image_h8i4w1h8i4w1h8i4.jfif", "scaleMode": "crop", "cropX": 128, "cropY": 199, "cropW": 660, "cropH": 971, "cropKeepAspect": true },
  "video": { "path": "C:\\Users\\Semen\\Downloads\\Gemini_Generated_Image_h8i4w1h8i4w1h8i4.jfif" },
  "autostart": true
}
```
(mediaMode=static → type=static; imagePath→static.path; mediaPath→video.path; crop*→static; autostart default true.)

## Риски
- VideoFileSource: ошибка декодирования убивает источник до следующего Open (по спеке); transient ReadSample-сбой = вечный false до переоткрытия хостом.
- Pacing в WriteFrame/FlushLast (Sleep внутри) — хост должен звать из своего цикла; при медленном Render кадры уходят позже тика.
- WholeProgramOptimization=true в статике → /GL obj (линкер сам перезапускает с /LTCG — проверено).
- Два продьюсера одновременно пишут одну секцию (старый exe + новый хост) — не проверялось, hot-switch сегрегация не тестировалась (нет хоста).
- Letterbox для видео — билинейный (как в VideoProducer), НЕ HighQualityBicubic (GDI+ в видео не использовался и в оригинале).

## Запрос
- Не требуется.
