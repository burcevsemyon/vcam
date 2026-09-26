# vcam-video-preview (subagent 2 memory)

Задача: приложение `VCamPreview` — always-on-top окно предпросмотра кадра из общей памяти.
**Статус: done** (приёмка 1-7 зелёная, 0 раундов фиксов).

## Scope
Только VCamPreview.cpp/.vcxproj/.filters + запись в .sln. НЕ трогал: StaticProducer, VideoProducer, MediaSource, CaptureTest, settings.json.

## Что сделано (файлы)
- `src/VCamPreview/VCamPreview.cpp` — один .cpp, Win32 GUI (wWinMain, subsystem WINDOWS), UNICODE, без MFC/ATL; линкуется только дефолтными user32/gdi32 (+CRT).
- `src/VCamPreview/VCamPreview.vcxproj` — клон StaticProducer.vcxproj: Release|x64, v145, stdcpp20, MT-CRT, `_WINDOWS` (не `_CONSOLE`), SubSystem=Windows, GUID `{BD0A8099-A996-4EDF-BA33-C1130004C78A}`, OutDir `build\x64\Release\`, IntDir `build\obj\VCamPreview\...`.
- `src/VCamPreview/VCamPreview.vcxproj.filters`.
- `VirtualCameraMediaSource.sln` — добавлен проект + Release|x64 ActiveCfg/Build.0.

## Как работает (факты)
- Чтение: `OpenFileMappingW(FILE_MAP_READ)` на `Global\VCam.FrameBuffer.v1` (fallback `Local\...`), `MapViewOfFile(...,0)` = вся секция; размер проверяется через `VirtualQuery` RegionSize → защита от AV при кривом header.
- Ретрай подключения каждые 1000 мс, если секции нет (первый запуск без провайдера → NO SIGNAL, не падает).
- Seqlock как в `SharedMemoryFrameSource::AcquireFrame`: читаем `seq` до/после копии, нечётный → YieldProcessor-spin; idx >= slotCount / невалидный header → отказ. Копия в свой staging (плотные строки w*4), рисование из staging.
- Liveness (важно): жив = haveFrame && (GetTickCount64 - момент последней смены seq) <= 1500 мс. Если seq замер >3000 мс → `Disconnect()` (unmap+close), чтобы открыть НОВУЮ секцию после рестарта провайдера — иначе наш handle навечно держит мёртвый mapping и новый провайдер не подхватился бы (проверено вживую: реконнект через ~6 c работает).
- ValidateHeader: magic/version, w/h <=4096, stride >= w*4, pixelFormat==RGB32, frameSize <=64MB, slotCount 1..64, `sizeof(header)+slotCount*frameSize <= RegionSize`.
- Рендер: двойная буферизация (CreateDIBSection top-down 32bpp по размеру клиента) → PatBlt BLACKNESS → `StretchDIBits` (biHeight=-h, letterbox с сохранением пропорций, полосы чёрные) → BitBlt. WM_ERASEBKGND=1. Таймер SetTimer 33 мс → Tick (чтение + InvalidateRect, без стирания).
- Текст NO SIGNAL: шрифт из SPI_GETNONCLIENTMETRICS (Segoe UI), DrawText DT_CENTER|DT_VCENTER поверх чёрного фона.
- Заголовок: `VCam Preview — {w}x{h}@30` (30 = 10_000_000/VCamFrameInterval100ns из контракта) / `VCam Preview — нет сигнала`; меняется только при смене состояния (без мерцания).
- Окно: 640×360 клиент (AdjustWindowRectEx), WS_OVERLAPPED|CAPTION|SYSMENU|THICKFRAME, WS_EX_TOPMOST, старт — правый верхний угол work-area монитора под курсором (не центр). WM_NCHITTEST: 8px от краёв → HT* (ресайз мышью), остальное клиент → HTCAPTION (перетаскивание), WM_NCLBUTTONDBLCLK → сброс 640×360, WM_GETMINMAXINFO мин 160×90, Esc / Ctrl+Q → DestroyWindow. Topmost пере-утверждается в Tick, если вдруг снят.

## Приёмка (факты, цифры)
1. MSBuild Release|x64 **exit=0**, `build\x64\Release\VCamPreview.exe` 152 576 байт.
2. Провайдер `VideoProducer.exe e2e_output\pattern_top_white.mp4` + Preview: окно найдено, заголовок `VCam Preview — 1280x720@30`, снимок области окна 656×399 (261 744 px): **nonBlack=117 611**, **white(>=250)=85 921** — белые полосы pattern видны. Снимок: `e2e_output\preview_alive.png`.
3. Убил свой VideoProducer: окно живо, процесс жив, заголовок `VCam Preview — нет сигнала`, снимок nonBlack=31 523 (текст NO SIGNAL), white=1. Снимок: `preview_nosignal.png`. Процесс VCamPreview не падает.
4. `GetWindowLongPtrW(GWL_EXSTYLE)` = 0x108, **WS_EX_TOPMOST (0x8) установлен**.
5. `e2e_test.ps1` **exit 0** (5 кадров, SUCCESS).
6. Бонус-проверка: рестарт провайдера → переподключение, заголовок снова `1280x720@30`, nonBlack=202 810 (`preview_reconnect.png`).
7. Свои тест-процессы (VideoProducer ×2, VCamPreview) убиты; чужих на момент старта не было, ничего чужого не трогал. Esc закрывает окно (проверено SendKeys) — остаточных процессов нет.
8. Этот файл заполнен; статус в `vcam-video.memory.md` обновлён.

## Риски
- Esc/Ctrl+Q требуют фокуса окна (клик по окну даёт его; окно — HTCAPTION, активация при клике работает).
- Живость определяется по инкременту `seq`; провайдер, пишущий без инкремента seq, будет считаться NO SIGNAL (по контракту такого не бывает).
- После смерти провайдера наш handle держит секцию до 3 с (затем Disconnect) — окно до этого показывает NO SIGNAL.
- Измеренный FPS в заголовке не выводится (показывается номинал 30 из контракта).
- Двойной клик по окну (не по краю) сбрасывает размер — это же поведение и есть по ТЗ.
