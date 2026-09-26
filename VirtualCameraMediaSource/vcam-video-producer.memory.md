# vcam-video-producer (subagent 1) memory — DONE

Scope: только `VideoProducer` (новый проект) + чтение новых полей settings.json в обоих провайдерах.
Не делал: VCamPreview, UI, звук, HW-ускорение.

## Память вышестоящей — ключевое
- Репо: `C:\Users\Semen\source\repos\VCam\VirtualCameraMediaSource`, sln `VirtualCameraMediaSource.sln` (Release|x64).
- MSBuild: `"C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" VirtualCameraMediaSource.sln /p:Configuration=Release /p:Platform=x64 /m /v:m /nologo`.
- Контракт: `src/Common/SharedMemoryContract.h` — `Global\VCam.FrameBuffer.v1`, `Global\VCam.FrameReady.v1`, 1280×720, stride 5120, RGB32(BGRX), `VCamFrameSize=3686400`, 8 слотов, seqlock, DACL `vcam::VCamDacSddl`.
- Settings: `%APPDATA%\VCam\settings.json`; старые поля `{imagePath, scaleMode, crop*}` + новые `mediaMode` (static|video), `mediaPath` (пишет UI).
- E2E: `e2e_test.ps1`; `CaptureTest.exe <n> <prefix>` → BMP 1280×720 24bpp (2 764 854 байт), prefix — путь без расширения.

## Что сделано (файлы)
- `src/VideoProducer/VideoProducer.cpp` (новый, ~700 строк): каркас StaticProducer (CreateFileMapping+хедер, writer 30 FPS slot=(idx+1)%8 под CS, seqlock, ready event, Escape, settings-polling 500ms + debounce lastWrite, stdout+fflush) + MF Source Reader decode-thread.
- `src/VideoProducer/VideoProducer.vcxproj` + `.vcxproj.filters` (копия StaticProducer: Release|x64, v145, MultiThreaded, stdcpp20, OutDir/IntDir те же; deps: advapi32, ole32, mfplat, mf, mfreadwrite, mfuuid; includes `$(ProjectDir);$(ProjectDir)..\Common`).
- `VirtualCameraMediaSource.sln`: проект `VideoProducer` GUID `{A72C3583-3881-4AC4-8384-422AA42C9FFC}` + Release|x64 Build.
- `src/StaticProducer/StaticProducer.cpp`: Settings += `mediaPath`/`mediaMode`; `EffectiveImagePath()` (mediaPath если задан и mediaMode!="video", иначе imagePath, иначе mediaPath); `LoadSettings` нормализует imagePath → downstream-код НЕ менялся; `WarnIfVideoMode()` в wmain и в watcher (смена mediaMode); crop/keepAspect логика не тронута; argv[1] по-прежнему переопределяет стартовый путь.

## Ключевые факты / питфолы MF (важно!)
1. **IMFSourceReader НЕ принимает голый `RGB32` → `MF_E_INVALIDMEDIATYPE (0xC00D36B4)`.** Работает только если reader создан с атрибутом `MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING=TRUE` (C++-метод `IMFAttributes::SetUINT32`, НЕ SetBOOL — его нет).
2. Порядок попыток в `OpenVideo`: passes = videoProcessing → plain → advanced; кандидаты формата: RGB32 1280×720 (только если исходное соотношение сторон ≈16:9±1%) → RGB32 native progressive → RGB32 native → RGB32 без размера → ARGB32 native. Победивший логируется `[video] output format: ...`.
3. `IMFSourceReader` не имеет `GetStreamCount` — перебор `GetNativeMediaType(i,0)` до ошибки; нет `SetPosition` — `SetCurrentPosition(GUID_NULL, PROPVARIANT{VT_I8, hVal=0})`; флаг `MF_SOURCE_READERF_ENDOFSTREAM` (без второго `_`).
4. `IMFMediaType` не имеет `SetMajorType/SetSubtype` → `SetGUID(MF_MT_MAJOR_TYPE/SUBTYPE)`.
5. Луп на EOS: SetCurrentPosition(0) + НЕ трогаем baseMs → ts=0 позади wall-clock → авто-resync (порог 250ms) → бесшовно; при провале seek — переоткрытие ридера.
6. Ориентация RGB32: `MF_MT_DEFAULT_STRIDE` пришёл = +1280 (top-down) — проверено pattern-роликом (top=191, bottom=0). `RowPtr()` поддерживает и negative stride (bottom-up).
7. Letterbox: свой билинейный 16.16 fixed-point в `LetterboxBilinear()`, чёрные полосы; для 320×240 → 960×720, pillarbox ровно 160px (замер: bright=75% семплов, полосы=0).
8. Источник VideoProducer: `desired = mediaPath (settings) ?? argv[1]`; смена → переоткрытие без перезапуска; ошибочный путь — лог и продолжение старого клипа (при неудаче без активного ридера — тихий ретрай).

## Тестовые файлы (оставлены, могут пригодиться субагентам)
- `e2e_output\test_video.mp4` (220 293 B) — testsrc2 320×240, 24fps, 5с — основной тест.
- `e2e_output\test_video2.mp4` (220 293 B) — второй такой же (hot-reload смена пути).
- `e2e_output\pattern_top_white.mp4` (3 606 B) — верх белый/низ чёрный — проверка ориентации/letterbox.
- Логи тестов: `vp_accept.log`, `vp_reload.log`, `vp_pattern.log`, `vp_smoke.log`, `sp_mode.log`, `sp_mediapath.log`.
- ffmpeg: WinGet Gyan.FFmpeg (доступен через `ffmpeg` в PATH).

## Приёмка (все выполнены)
1. ✅ Решение Release|x64 собирается, exit=0 (в т.ч. после финального коммита-порядка passes; e2e тоже собирает).
2. ✅ `VideoProducer.exe <test_video.mp4>` живёт; `CaptureTest.exe 3 <prefix>` → 3 BMP, непустые: 75% семплов яркость>10 (внешние 25% — pillarbox = 0).
3. ✅ Движение: MD5 соседних кадров различаются (`72315139763D` ≠ `99B3D84075CE` и т.д.).
4. ✅ Луп: процесс жив на t=12.3с (2 полных проигрывания, в логе 2× `end of stream - looping`); кадры на t≈2.3с и t≈7.3с непустые; acc_a_000 == acc_b_000 (тот же кадр через 5с = синхрон лупа).
5. ✅ Hot-reload: mediaPath→test_video2 → `[settings] mediaPath ->` + переоткрытие; mediaMode→static → `WARNING: mediaMode="static" ... - continuing anyway`; mediaPath убран → возврат на argv[1]. StaticProducer: mediaMode=video → warning + продолжает с imagePath; mediaPath+static → грузит mediaPath. **settings.json восстановлен байт-в-байт (сверено с исходным).**
6. ✅ `e2e_test.ps1` → exit 0 (прогнан дважды, последний — после финальной сборки).
7. ✅ Этот файл заполнен.

## Риски
- Зависимость от `MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING` — без неё RGB не принимается; порядок passes покрывает fallback, но на голом `plain`-ридере будет лог из 4 неудач (теперь videoProcessing первый → лог чистый).
- `SetCurrentPosition(GUID_NULL)` проверен только на H.264/mp4; иначе срабатывает fallback-переоткрытие.
- Смена ориентации возможна для других кодеков/interlaced (RowPtr корректен при отрицательном stride, но не проверен на них).
- Resync-порог 250ms: при фризе >250ms кадры пропускаются (осознанно).
- Если одновременно запущен чужой StaticProducer — два писателя в одну секцию (как и раньше); в тестах процессов-конкурентов не было.
- Новые поля в C++ только читаются; обратной записи нет (пишет UI, subagent 3).
