---
name: vcam-e2e
description: >-
  E2E-тесты VCam: e2e_test.ps1 (фазы A/B/G/H/C/D/E/F: CLI hot-switch, dual-write
  v1+v2, ladder 1080p, camera, device-режим через прокси FrameServer, fallback
  при остановке продьюсера; Wait-Ready readiness-gate вместо Start-Sleep),
  бэкап/restore settings.json, конфликт двух писателей, тестовые медиа
  e2e_output, assert движения/статики/letterbox по BMP, HKCU-регистрация CLSID
  (non-elevated regsvr32 — no-op); живая диагностика симптомов потребителя
  (ktalk), msrc_diag.log, деплой MediaSource.dll (ритуал FrameServer),
  диагностика падения producer'а. Use when: e2e_test, VCam e2e, CaptureTest,
  inspect/device mode, проверка виртуальной камеры, hot-switch тест, «два
  писателя в секцию», кадры не идут/идентичны, «картинка вытянута/только
  верх/статична», msrc_diag, deploy MediaSource, FrameServer stop, «нет
  сигнала».
---

# VCam E2E (проектный skill)

Пайплайн теста: **продьюсер** (`VCamProducerCli run`) → shared memory
`Global\VCam.FrameBuffer.v1` → **камера** (`MediaSource.dll`) → **захват**
(`CaptureTest.exe` → BMP) → **ассерты** (nonBlack + SHA256 хэши кадров).

Подробный скрипт: `VirtualCameraMediaSource/e2e_test.ps1`.
Память задач: `VirtualCameraMediaSource/*.memory.md` (`memory.md` + `observability-*.memory.md`;
читать память вышестоящей задачи перед работой).

## Перед прогоном (обязательно)

1. **Остановить tray-хост** `VCamVideoStreamProducer` (и CLI) — два писателя
   в одну секцию интерливят кадры. Скрипт теперь **падает (exit 1)**, если
   `VCamVideoStreamProducer`/`VCamProducerCli` запущены:
   `Stop-Process -Name VCamVideoStreamProducer,VCamProducerCli -Force`.
2. Камера зарегистрирована: `build\x64\Release\Registrar.exe add VCam hold`
   (elevated; процесс **не закрывать**, держит держку — проверка:
   `Get-Process Registrar` + `CaptureTest inspect` → `count=2`). **Elevation
   на самом деле НЕ нужен** (MFVirtualCameraAccess_CurrentUser, verified);
   hold-режим — вечный цикл с `[holder alive]` каждые 5 с, kill процесса =
   стоп камеры («will NOT remove on exit»). **Хост тоже владеет холдером**
   (vcam-camera-lifecycle): он сам запускает `Registrar.exe add VCam hold-watch`
   при старте и `taskkill` при tray «Выход» — **но только если потребителей нет**
   (heartbeat `readerLastActiveTick` в секции, порог 3 с): с активной
   MF-сессией (ktalk и т.п.) хост останавливает только писателя и логирует
   `consumers active - camera kept`, камера продолжает отдавать NO SIGNAL
   (через 7 с после стопа писателя), а hold-watch сам выходит, когда секция
   и heartbeat молчат ≥30 с (проверка каждые 5 с). Ручное включение хоста
   перед прогоном e2e НЕ требуется — держить холдера самому (команда выше,
   hold подходит). После каждой остановки/рестарта
   FrameServer и после чистки процессов перед прогоном — убедиться, что
   Registrar жив, иначе фазы D/E падают с «VCam device not enumerated»
   (inspect count=1). **Автозапуск**: HKCU\Run\VCamRegistrar **УДАЛЁН**
   (<=0.0.2; install-чистка в ssPostInstall) — камеру даёт хост из задачи
   VCamHost; для ручного прогона без хоста держать холдер живым самому. Скрипт дополнительно делает
   `regsvr32 /s MediaSource.dll` — **из НЕ-elevated шелла это no-op**
  (exit 5, E_ACCESSDENIED, HKLM не пишется); поэтому скрипт регистрирует
   build-путь ещё и в **HKCU** (`HKCU\Software\Classes\CLSID\{B2B674D4-…}\InprocServer32`
   — HKCU приоритетнее HKLM, verified bogus-тестом → 0x8007007E) и убивает
   ключ в `finally`. Установленная копия `C:\Program Files\VCam\MediaSource.dll`
   используется device-режимом (прокси FrameServer) — скрипт сверяет её SHA256
   со сборкой и при расхождении SKIPает фазы D/E («сначала деплой»).
3. Тестовые медиа: `e2e_output\test_video.mp4` (5 с, testsrc2 320x240;
   `test_video2.mp4`, `pattern_top_white.mp4` — для hot-reload/ориентации).
   `test_input.bmp` лежит в корне `VirtualCameraMediaSource/` (в репо) —
   внешние файлы не нужны.
4. MSB3027 — закрыть свои exe перед сборкой (скрипт сам собирает solution).

## Запуск

```powershell
cd VirtualCameraMediaSource
powershell -ExecutionPolicy Bypass -File e2e_test.ps1   # exit 0 = SUCCESS
```

- **Фаза A** — `run --type video --path <test_video.mp4>` (overrides):
  кадры должны ДВИГАТЬСЯ (unique SHA256 > 1), в логе `writer ready` и
  `[cli] active:`, `status` → «frames are being written», BMP строго 640×480
  с letterbox.
- **Фаза B** — `run` БЕЗ overrides + перезапись settings.json:
  static → кадры идентичны (unique = 1) → video → движутся; в логе
  ≥2 × `[cli] switch:` и `[cli] source opened: type=video` (проверка
  hot-switch без перезапуска, через SettingsWatcher); `status` растёт.
- **Фаза G** — dual-write v1+v2 (video, moving): `status` показывает обе секции
  (`frames are being written` для v1 и v2); заголовки секций: v1 ver=1 1280×720,
  v2 ver=2 (dims по quality).
- **Фаза H** — ladder: static 1080p-источник рекламирует нативный 1920×1080
  (WriteFrameNative); quality hot-switch `fixed720p`/`fixed1080p` без рестарта
  (`[cli] switch: ... quality=fixed720p|fixed1080p` в логе, v2 dims следуют).
- **Фаза C** — физическая камера (list-devices + `run --type camera`, негатив:
  несуществующий id → NO SIGNAL); камера занята → SKIP.
- **Фазы D/E** — device-режим (`CaptureTest device <idx> W H`) через прокси
  FrameServer = путь ktalk: индекс VCam ищется по `inspect` (mediaTypes=3),
  D = 1280×720 RGB32 (ассерты: полная высота кадра, `negotiated … RGB720`,
  `Lock maxLen=3686400`, нет `buffer too small`), E = 640×480 RGB32
  (letterbox + контент-строки, `negotiated … RGB640`, `Lock maxLen=1228800`).
  Занятая сессия (0xC00D3E9B) → SKIP; установлена ≠ сборка → SKIP.
- **Фаза F** — остановка CLI → 3 кадра идентичны (fallback: кэш последнего
  кадра в пределах **7 с** после последнего свежего, дальше NO SIGNAL —
  vcam-camera-lifecycle; сразу после остановки кэш ещё в окне, не чёрные)
  → рестарт → кадры снова движутся.
- **finally** — settings.json восстанавливается **байт-в-байт**
  (SHA256-сверка); если файла не было — тестовый удаляется. Нарушение
  SHA256 = FAIL. HKCU-ключ CLSID удаляется/восстанавливается.
- Артефакты: `e2e_output\` (BMP `prefix_*.bmp`, `cli_phase_*.log`,
  `cli_phase_*.err`, `capture_*.log`, `inspect_vcam.log`).

## Ассерты (как считаются)

- **nonBlack**: сэмпл BMP каждые 32×18 px, значения >10 по любому каналу;
  max > 0 — значит продьюсер пишет (0 = чёрный экран = кадров нет).
- **Движение/статика**: SHA256 каждого кадра; video → уникальных > 1,
  static → ровно 1.
- CaptureTest direct: `CaptureTest.exe <n> <prefix>` → (после фикса
  SetMediaType) негоцирует **NV12 640×480** → BMP 640×480 24-бит
  (921 654 байт), сверху/снизу letterbox-полосы. Старые записи «BMP 1280×720
  2 764 854 байт» — поведение ДО фикса (SetMediaType падал → фолбэк на
  дефолтный тип 720p); e2e теперь ассертит именно 640×480.
- Letterbox-ассерт (фазы A/B/E): строки y∈{5,25,55} и y∈{425,455,475} строго
  чёрные (max=0), контент-строки y∈{75,240,410} non-black — регрессия
  «вытянута по вертикали». 720p (фаза D): центральная колонка x=600..680 в
  строках y=5 и y=714 non-black — регрессия «только верхняя часть».
- Лог захвата: `e2e_output\capture_<prefix>.log`; device-режим пишет ещё
  `capture_<prefix>.err`.

## Питфолы (это ломалось на практике)

- `%APPDATA%` внутри printf-строки C++ → invalid parameter handler →
  exit `0xC0000409`. Только `%%APPDATA%%`.
- JSON-ключ матчить **только с `:` после него** (баг FindKeyPos): иначе
  `"type":"video"` (значение) ловится как имя секции → читается static.
  Симптом: type=video, а кадры идентичны / `status` показывает video.path
  от static.
- PowerShell 5.1: `Set-Content -Encoding UTF8` пишет **BOM**. Для settings:
  `[IO.File]::WriteAllText($path,$json,(New-Object System.Text.UTF8Encoding($false)))`.
- Overrides `--type/--path` фиксируют источник на весь запуск и игнорируют
  правки settings.json — честный hot-switch тестируется только без
  overrides (фаза B).
- CLI из скрипта: `Start-Process -PassThru -RedirectStandard*`, остановка —
  `Stop-Process` + `WaitForExit`; готовность продьюсера — `Wait-Ready`
  (poll `status --ready` каждые 500 мс до 10 с, `ready in ~1s`), НЕ `Start-Sleep 4`
  (механика P1.3, skill `ps-wait-ready`). Остались sleep на hot-switch (3 с),
  NO SIGNAL (7 с) и debounce — они не readiness, не трогать.
- Единственный writer на время теста = CLI; после — убедиться, что процесс
  убит (`finally` в скрипте это делает, но при ручных запусках — проверить).
- **regsvr32 из НЕ-elevated шелла = no-op (exit 5, E_ACCESSDENIED)**: HKLM не
  пишется, а CoCreateInstance резолвит HKCR→HKLM →
  `C:\Program Files\VCam\MediaSource.dll` (УСТАНОВЛЕННАЯ копия, не build) —
  ручные пробы и direct-фазы молча тестируют старую DLL. Решение (в e2e):
  пер-пользовательская регистрация
  `HKCU\Software\Classes\CLSID\{B2B674D4-9CF0-461C-BDCE-3D56FBB41356}\InprocServer32`
  = путь build; **HKCU приоритетнее HKLM** (проверено bogus-тестом →
  0x8007007E, а не загрузка HKLM-копии). Для svchost (SYSTEM) HKCU не
  действует → device-режим грузит установленную копию: сверять SHA256
  build vs `C:\Program Files\VCam\MediaSource.dll` (e2e это делает).
- **Фильтр имени `VCam` в device-режиме НЕ матчится**: FRIENDLY_NAME пуст и у
  VCam, и у реальной камеры → фолбэк «first unnamed» берёт device[0] =
  РЕАЛЬНУЮ камеру (метка в логе: «No device matched filter … accepting first
  device without FRIENDLY_NAME»). Индекс искать через `CaptureTest inspect`:
  VCam = устройство с `mediaTypes>=3` И 720p И 640p-типами (логика `Find-VCamDevice`
  в e2e: `mt -ge 3` + `has720` + `has640`; сейчас 4 типа — RGB32/NV12 720p + RGB32
  640p + ladder; было 3 — не матчить ровно 3); числовой фильтр = принудительный индекс
   (`CaptureTest device 1 640 480 out`).
- **Negative probe фазы H (`Find-VCamDeviceNative`) scoped по индексу VCam**:
  она матчила `size=1920x1080` по ЛЮБОМУ устройству → физ. камера (Brio, 339
  типов, легитимный 1920×1080) давала ложный FAIL «ladder advertises native»
  (08.10.2026). Теперь probe получает индекс из `Find-VCamDevice` и ищет натив
  только внутри типов этого устройства; VCam-устройство не найдено → честный
  FAIL «not enumerated», а не тихий пропуск.
- **Direct-режим НЕ использует shared allocator** (без прокси m_pAllocator =
  null → `local buffer (no shared allocator)`) — строк `Lock maxLen=` и
  `buffer too small` в логе не будет. Ассерты аллокатора проверяются только
  в device-фазах D/E.
- Строка `deliver type … (selected=…)` логируется лишь на смену типа и в
  первой device-сессии наблюдалась отсутствующей — как обязательный ассерт
  НЕ использовать; надёжные маркеры прокси-пути: `negotiated … allocator
  type RGB720/RGB640` и `Lock hr=0x00000000 … maxLen=…`.
- UI round-trip тестов постоянного проекта НЕТ: воссоздаются временным
  net10.0-windows проектом в `%TEMP%` (инструкция по воспроизведению — в памяти
  соответствующей задачи, в репо как файл не хранится); тестовый файл
  `%TEMP%\vcam-ui-roundtrip-data\settings.json`, рабочий
  `%APPDATA%\VCam\settings.json` не трогать.
- Быстрая живая диагностика без скрипта:
  - `VCamProducerCli.exe status` — seq растёт = пишется (status сам не пишет);
  - скриншот окна `VCamPreview` + mean яркость (640×360): статика ≈ 48,
    NO SIGNAL ≈ 15 — надёжно отличает фазы;
  - остановка хоста: named event `VCamVideoStreamProducer.Stop` → `Set()`
    (так же, как кнопка в UI);
  - stop/start: после рестарта хоста превью возвращается ≤ ~4 с
    (окно реконнекта 3 с + retry 1 с).

## Связанные константы контракта

- 1280×720 BGRX, stride 5120, 8 слотов, seqlock; fallback: нет свежего
  кадра 40 мс → кэш последнего кадра, но **не дольше 7 с** после последнего
  свежего кадра (vcam-camera-lifecycle) → дальше NO SIGNAL; без кэша — сразу
  NO SIGNAL.
- Тайминги хоста: hot-switch окно 5 с (FlushLast), Preview: timeout 1.5 с,
  reconnect 3 с.

## Живая диагностика: симптом → причина (эмпирика из ktalk-инцидентов)

Симптомы описывает потребитель (например, ktalk через прокси FrameServer);
наша DLL грузится в svchost сервиса FrameServer, НЕ в потребителе.

| Симптом | Причина | Быстрая проверка |
|---|---|---|
| «картинка только в верхней части, ниже чёрное» | доставляемый размер ≠ размер буфера аллокатора (640×480 данные в 720p-буфере) | в msrc_diag.log: `StartForSession negotiated … -> allocator type …`, `Lock … maxLen=` (1228800=640×480 RGB32, 3686400=720p RGB32, 1384448=NV12 720p) |
| «вытянута по вертикали» (1.33×) | неравномерный downsample 16:9→4:3 в `DownsampleRgb32` (MediaStream.cpp) — лечится letterbox-вписыванием | код: `DownsampleRgb32`; симптом появляется ТОЛЬКО когда размеры совпали (предыдущий баг маскировал) |
| «статична» / кадры не обновляются | встал producer (камера/источник), НЕ media source; если producer мёртв дольше 7 с — кадр сменится на NO SIGNAL (vcam-camera-lifecycle) | `VCamProducerCli status` → «no new frames»; или напрямую seq в `Global\VCam.FrameBuffer.v1` (см. ниже) |
| «камера не подключена» | баг потребителя (его выбор устройства), на нашей стороне не лечится | — |
| НЕ причина: окно превью | `VCamPreview` — чистый read-only читатель shm (`FILE_MAP_READ`), в MF/камеру/писателя не вмешивается | код VCamPreview.cpp:125 |

### msrc_diag.log — что смотреть

- Путь: `%ProgramData%\VCam\msrc_diag.log` (общий для обоих контекстов; ACL
  `LOCAL SERVICE`/`NETWORK SERVICE`/`Users` = Modify выдаёт инсталлятор или
  deploy-ритуал) плюс `<GetTempPath>\VCam\msrc_diag.log` процесса, загрузившего
  DLL (юзер → `%TEMP%\VCam\`, svchost → свой temp). **Без выданного ACL svchost
  пишет молча в никуда**: наследованный от ProgramData/Temp даёт LOCAL SERVICE
  только чтение — device-строки теряются (проверено 05.10.2026). e2e читает
  окно по обоим файлам. Cap 10МБ + ротация в `.old` (P0.4); формат единый P0.1
  с датой: `[YYYY-MM-DD HH:MM:SS.mmm] [info] [msrc] pid=.. tid=.. ...`
  (старые сборки без даты/`-> RGB32` — НЕ текущая DLL). Отфильтровать
  по `pid=` актуального svchost (новый pid появляется после каждого рестарта
  FrameServer). Скрипт e2e не читает весь файл: фиксирует offset до фаз
  и потом читает только окно (`Read-DiagWindow`).
- Ключевые строки (текущая сборка): `Stream.SetMediaType -> WxH fmt`,
  `SetMediaType handler SetCurrentMediaType hr=` (синк SD handler),
  `StartForSession negotiated WxH fmt -> allocator type …`,
  `DeliverNextSample deliver type WxH fmt (selected=…)` (одна на смену типа
  за сессию), `Lock hr=0x0 maxLen=… curLen=…` (проверка реального размера
  буфера), `before AcquireFrame` / `AcquireFrame done` (пары = тайминг ожидания кадра).
- **AcquireFrame ≈ 45 мс (пик 40–46) = таймаут 40 мс → кадров из shm нет.**
  Норма при живом писателе:AcquireFrame возвращается быстро, cadence ≈ 33 мс.
- `Lock` пары: `curLen` у аллокатора перетирается под init-тип при каждой
  выдаче — по нему ФАКТИЧЕСКИЙ размер доставок определить нельзя, только
  по `maxLen` (тип аллокатора).
- Утечки/чистки: dtor обязан релизить все media type (включая запасные
  NV12/640) — иначе утечка на каждый переключатель типа.

### Падение producer'а («Нет сигнала» в тултипе трея)

- seq в shm заморожен → писатель встал; потребитель при этом продолжает
  отдавать **кэш** последнего кадра (FallbackFrame) → «статичная картинка».
- Диагностика: `VCamProducerCli.exe status` («no new frames in the last
  300 ms (seq N)») либо прямое чтение заголовка секции (offset: 9×uint32 →
  `seq` int64; PowerShell `MemoryMappedFile.OpenExisting(Global\VCam.FrameBuffer.v1, Read)`).
- Причина из лога хоста: хост пишет `%LOCALAPPDATA%\VCam\host.log` (файл, единый
  формат P0.1; читается живьём) — рестарт с перехватом stdout больше не нужен.
  Смотреть хвост (`no signal: …`, `config applied`, `previous run did not shut down
  cleanly` для прошлых падений).
- Известное: `ReadSample failed: 0x80070428` (ERROR_SERVICE_DISABLED) от
  физической камеры **после остановки FrameServer** — лечится просто
  рестартом хоста (open→active занимает ~8 с; в Fallback ретраи идут раз в 1 с,
  Render-ошибки там НЕ логируются — молчание лога ≠ зависание).
- Остановка хоста: named event `VCamVideoStreamProducer.Stop` → `Set()`
  (кнопка UI); kill `Stop-Process -Name VCamVideoStreamProducer` — приемлемо
  для тестов; autostart: задача `Task Scheduler\VCamHost` (`schtasks /Query /TN VCamHost`;
  создание/удаление требует прав; `HKCU\Run\VCamAutostart` — legacy, больше нет;
  hост-лог-маркер: `autostart enabled (Task Scheduler\VCamHost)`, токен —
  `token: elevated=1 SeCreateGlobalPrivilege=2`).
- Системная диагностика средствами наблюдаемости (`status --json`, счётчики,
  `config applied`, crash-маркер, `diag`-пакет) — skill `vcam-diag` (здесь выше —
  только e2e/ktalk-специфика).

## Деплой MediaSource.dll (ритуал)

DLL в `C:\Program Files\VCam\MediaSource.dll` занята сервисом FrameServer
(svchost). Обычные `Stop-Service`/`sc stop` из НЕ-elevated шелла → отказ в
доступе (ошибки в логе искажены CP1251). Ритуал:

1. **Сборка**: MSBuild solution Release|x64, exit 0 (см. AGENTS.md проекта).
2. **Закрыть ktalk (и всех клиентов камеры!)** — иначе `sc stop FrameServer`
   висит в STOP_PENDING, SCM откатывает обратно в Running, копирование
   пропускается молча (проверять dst time == src time!).
3. Elevated-скрипт (`%TEMP%\opencode\elev_fs_stop.ps1` или аналог) через
   `Start-Process -Verb RunAs -Wait` (без `-RedirectStandardOutput`):
   `sc stop FrameServer` → цикл ожидания Stopped → `Copy-Item` (fallback
   rename+copy) → `sc start FrameServer` → запись лога (state, COPY OK,
   dst size/time).
4. **Верификация**: лог содержит `COPY OK`; `dst size/time == src`;
   `Get-Service FrameServer` = Running; **новый pid** svchost (старый умер);
   `VCamProducerCli status` = frames are being written.
5. Смены DLL видны потребителям только после рестарта сервиса (грузится
   прокси-потребителями заново). Страницу «Камеры» Windows Settings при
   необходимости перезапустить отдельно.

## Чек-лист «всё работает» (после любого деплоя/рестарта)

1. `Get-Service FrameServer` = Running, DLL dst time == src time.
2. `VCamProducerCli status` → «frames are being written (seq N → M)», seq растёт.
3. msrc_diag.log: свежие строки `negotiated … -> allocator type …` и
   `Lock … maxLen=` (при желании `deliver type … (selected=…)`, но она может
   отсутствовать), размеры совпадают; AcquireFrame быстрый (не ~45 мс).
4. Потребитель: картинка полного размера, пропорции 16:9 (без вертикального
   растяжения), обновляется (движется).
