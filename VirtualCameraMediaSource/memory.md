# VirtualCameraMediaSource — session memory (resume doc)

Purpose: durable record so an interrupted agent can resume. Update as work progresses.

## LATEST SERIES (27.09.2026) — INSTALLED: VCam Installer v0.0.1 (Inno Setup)
- Created Inno Setup installer script (`vcam_installer.iss`) for version `0.0.1`.
- Built all C++ components (`Release|x64`) and published `VCamSettingsUi` as self-contained x64 (`win-x64`).
- Implemented robust per-machine setup logic:
  - Stops `FrameServer` service during update/uninstall to prevent file locks on `MediaSource.dll`.
  - Registers COM `MediaSource.dll` via `regsvr32.exe`.
  - Configures current user `HKCU\Run` autostart for tray host (`VCamAutostart`) and registrar holder (`VCamRegistrar` via hidden PowerShell wrapper).
  - Automatically launches the registrar holder and tray host post-install so the camera is immediately active (`inspect count=2`, frames active).
  - Clean uninstallation unregisters COM, stops processes, restores FrameServer service state, and preserves user settings (`%APPDATA%\VCam\settings.json`).
- Compiled artifact: `VCamSetup-0.0.1-x64.exe`.

## LATEST SERIES (26.09.2026 ~22:30) — RESOLVED: ktalk «вытянута по вертикали + статична» → letterbox + рестарт хоста; пользователь подтвердил «всё в порядке»
- Симптом после предыдущих 4 фиксов: «картинка вытянута по вертикали на весь экран и статична».
- Диагноз по частям:
  1. **«Статична» = встал producer (tray-хост), НЕ media source.** Доказано: seq в Global\VCam.FrameBuffer.v1
     заморожен (мерили 12 мин), `VCamProducerCli status` → «no new frames», у потребителя каждый
     AcquireFrame = таймаут 40 мс → FallbackFrame (кэш) → одна картинка. Причина первого падения:
     тултип «Нет сигнала» — камера Brio перестала отдавать кадры (ReadSample failed 0x80070428 =
     ERROR_SERVICE_DISABLED после остановки FrameServer). Лечение: рестарт хоста (`host_out2.log`
     перехват stdout: switch→source opened→active).
  2. **«Вытянута по вертикали» = аспект 16:9→4:3.** Размеры к теперь совпадали (deliver type 640x480 =
     selected, Lock maxLen=1228800), но `DownsampleRgb32` жал 1280x720→640x480 неравномерно
     (x*0.5, y*0.667) → растяжение 1.33x. Фикс: letterbox в `DownsampleRgb32` (MediaStream.cpp:568) —
     вписывание с сохранением пропорций + чёрные поля (memset), центрирование.
  3. **Окно превью — НЕ причина** (проверено по коду): VCamPreview читает shm через
     FILE_MAP_READ, в MF/камеру/запись не вмешивается.
- Ритуал деплоя в этом раунде: ktalk держал FrameServer (sc stop висел в STOP_PENDING → SCM откатил);
  после закрытия ktalk elevated `elev_fs_stop.ps1` → стоп за ~0.8 с → COPY OK → старт. DLL 1465856 B
  22:20:57. **Правило: перед ритуалом закрывать ktalk (иначе sc stop не пройдёт).**
- Сборка Release|x64 exit 0 (изменён только MediaStream.cpp).
- **ИТОГ: пользователь «отлично, сейчас всё в порядке».** Сервис Running, писатель 30 fps, ktalk работает.
- Незакоммичено (коммит/пуш не делались, push запрещён): CaptureTest/main.cpp,
  Common/SharedMemoryFrameSource.cpp, MediaSource/MediaStream.{h,cpp} (все 5 фиксов + letterbox),
  ProducerCore/FrameWriter.cpp.
- Открытые мелочи (не мешают): (a) stdout текущего хоста никуда не пишется, если запущен не через
  redirect — причины падений камеры видны только через `host_out2.log` при рестарте с перехватом;
  (b) 0x80070428 после стопа FrameServer — штатно лечится рестартом хоста; (c) SHARED-инстанс —
  один m_selected* на всех клиентов (залогируется deliver type).

## LATEST SERIES (26.09.2026 ~21:44) — SYMPTOM: ktalk «картинка только в верхней части, ниже чёрное» → 4 фикса в MediaStream применены, DLL перезаложена, ОЖИДАЕТСЯ проверка в ktalk
- Симптом (пользователь): в ktalk изображение занимает только верхнюю часть экрана, ниже чёрное
  (= клиент читает 720p-строки из буфера, в который записан кадр меньшего размера: первые ~240
  строк валидны, остальные ноль/мусор).
- Доказательства (msrc_diag.log, ~266 МБ, pid svchost=25000 = ОДИН SHARED-инстанс на всех клиентов):
  - Единственный FinalConstruct 20:04:13; лента SetMediaType: 720p RGB32 → 640x480 RGB32 (20:04:43)
    → NV12 720p (20:23:30) → 640x480 RGB32 (20:32:19) → NV12 720p (20:39:08) → 640x480 RGB32 (20:46:04).
  - **StartForSession БЕЗ предшествующего SetMediaType: 20:32:42, 20:38:18, 20:53:29** — сессии
    стартовали со stale m_selected* (640x480 от 20:46:04). Если прокси выставил тип через SD handler
    напрямую, наш код это игнорировал (handler читался только для NV12-детекта) → рассогласование.
  - Lock-пары ВСЕГДА (3686400,3686400) и (1382400,1384448); 1228800 не встречается — аллокатор
    перетирает currentLength при каждой выдаче, поэтому по логу размер доставок НЕ определить
    (старая гипотеза «доставки всегда 720p» по логу неподтверждаема).
  - Сессия SetMediaType→640x480 (20:46:04) показывает Lock maxLen=3686400 — аллокатор был
    инициализирован 720p-типом (старый pInitType брал только NV12-или-720p).
- ФИКСЫ (26.09.2026, MediaStream.{h,cpp}, +MSBuild Release|x64 exit 0, DLL 1465856 B 21:44:27):
  1. `SetMediaType` дополнительно синкает SD handler: `pHandler->SetCurrentMediaType(pMediaType)`.
  2. Новый `CMediaStream::ResolveNegotiatedType(w,h,nv12)`: handler current type (subtype+frame size)
     = первоисточник, fallback m_selected*; валидация «только 1280x720 / 640x480»; используется в
     DeliverNextSample И StartForSession (убрана асимметрия «NV12 читаем, размер нет»).
  3. `StartForSession`: pInitType по (w,h,subtype) — NV12 720p / **m_pMediaType640** / m_pMediaType
     (раньше 640x480 всегда получал 720p-аллокатор).
  4. `Lock`: порядок аргументов `(pcbMaxLength, pcbCurrentLength)` исправлен, условие `maxLen >= needed`,
     else-ветка: zero maxLen + `SetCurrentLength(0)` + лог «buffer too small» (раньше был no-op).
  - Диагностика: лог разовый на смену типа `Stream.DeliverNextSample deliver type %ux%u %s
    (selected=…)` (дедуп per-session, сбрасывается в StartForSession) + `allocator type …` в Start.
  - Попутно: dtor/ShutDownInternal теперь релизит m_pMediaTypeNv12/m_pMediaType640 (был утечек).
- Deploy: обычный elevate-скрипт (Stop-Service без прав) не сработал; успешный ритуал =
  `%TEMP%\opencode\elev_fs_stop.ps1` elevated (sc stop FrameServer → COPY OK → sc start),
  log `%TEMP%\opencode\elev_fs_stop.log`. Старый svchost 25000 GONE, сервис Running, DLL 21:44:27.
- СТАТУС: сборка+деплой DONE; **ждём: пользователь открывает ktalk, воспроизводит симптом**, затем
  читаем новые строки `deliver type` / `allocator type` / `handler SetCurrentMediaType` в msrc_diag.log.
- Остаточные риски: (a) SHARED-инстанс — два одновременных клиента с разными типами не лечится
  (залогируется строкой deliver type); (b) letterbox в DownsampleRgb32 (16:9→4:3 растягивается, не
  чёрными полосами) — НЕ делался, отдельным шагом по запросу; (c) кейс CaptureTest device-mode
  0xC00D3E9B и «камера не подключена» — баги потребителя, без изменений.
- Незакоммичено (5 файлов, коммит/пуш не делались): CaptureTest/main.cpp, Common/SharedMemoryFrameSource.cpp,
  MediaSource/MediaStream.{h,cpp}, ProducerCore/FrameWriter.cpp.

## LATEST SERIES (13.09.2026 ~23:03-23:25) — FINAL: fix VERIFIED end-to-end (20/20 frames, zero FrameServer crashes); repo moved to C:\Users\Semen\source\repos\VCam (git init -b main + initial commit)
- Subagent retest 23:03 (after SharedMemoryContract.h:12 fix; DLL 234,496 B deployed 22:59:12):
  (a) no producer: 10/10 black frames, all MD5-identical (DC33A7301ED19E0133E639A9BAAA39BF) — fallback path no longer crashes;
  (b) producer: 10/10 real frames, 6 unique MD5s (animated pattern). BMPs 2,764,854 B (1280×720 24-bit):
  C:\Users\Semen\AppData\Local\Temp\opencode\r2fix_frames\. No new WER svchost_FrameServer crashes.
- WER LocalDumps key removed (dump kept: C:\ProgramData\VCamWerDumps\svchost.exe.29892.dmp, 117 MB).
- 23:12 re-registration: r2r_add.ps1 elevated non-blocking `Registrar.exe add VCam hold` (Registrar pid 26484).
  inspect count=2: device[0]=Brio 90 (339 types, 640x480 YUY2 current), device[1]=ours (2 types: NV12+YUY2 1280x720@30).
  PITFALL: `CaptureTest.exe device VCam r2final` (23:12) fell back to device[0]=Brio (both FRIENDLY_NAME='(none)')
  → 0xC00D3E9B there is a red herring about Brio, NOT our camera. Always force index: `device 1`.
- 23:18 retest `CaptureTest.exe device 1 <r2final2>`: 10/10, curLen=3686400, all sha256=c5e80e75… (black, no producer),
  Stop S_OK, exit 0. Registrar alive.
- User observation "saw a frame from vcam once, then an error in camera settings" = exactly the pre-fix signature:
  producer alive → in-bounds row-copy → frame visible; 40 ms ready-timeout / 2.86 MB OOB corruption → fallback memset
  → FrameServer AV → Settings shows camera error. Post-fix: no svchost crashes after 22:59:12; only nearby WER is
  SystemSettings.exe 23:07:19 (SystemSettingsViewModel.Desktop.dll) — Settings UI crash, not our DLL.
- User hint: cdb system symbols — `srv*C:\Symbols*https://msdl.microsoft.com/download/symbols`.
- Narrative correction: "environment blocks all vcams" (control experiment) was WRONG — collateral damage: our DLL's
  FrameServer crash took camsvc down for everyone (incl. reference VCamSample). After clean camsvc+FrameServer reset
  reference streams 10/10 on this build (26200.9445). Root cause = our VCamFrameSize overflow, fixed.
- REPO (user request): killed Registrar (pid 26484; Session lifetime → camera unregistered), moved project to
  C:\Users\Semen\source\repos\VCam, new README.md (Russian), git init -b main + initial commit (build/, *.pdb etc. ignored).
  Restore camera: elevated `VCam\build\x64\Release\Registrar.exe add VCam hold` (keep the process alive).

## PRIOR SERIES (13.09.2026 ~22:11) — MINIDUMP R1: DONE — ROOT CAUSE PROVEN: VCamFrameSize constant is 6,553,600 (width×stride) instead of 3,686,400 (height×stride); FallbackFrame memset overflows the 3,686,400-byte frameserver sample buffer → AV → FrameServer crash. R2 fix = 1 line in SharedMemoryContract.h
R1 setup: WER LocalDumps for svchost.exe (elevated): DumpType=2, DumpFolder=C:\ProgramData\VCamWerDumps,
ACL FULL for LOCAL SERVICE+SYSTEM. Repro = `Registrar.exe add VCam hold` (NO producer) → crash ~20s in.
Dump: **C:\ProgramData\VCamWerDumps\svchost.exe.29892.dmp** (FrameServer svchost pid 29892, 22:11:33).
Analyzer: WinDbg Preview cdb `C:\Program Files\WindowsApps\Microsoft.WinDbg_1.2606.22001.0_x64__8wekyb3d8bbwe\amd64\cdb.exe`
(Windows Kits Debuggers\x64 has NO cdb.exe here), PDBs from build\x64\Release (path validation OK).
- Exception: **0xC0000005 AV**, thread 0:011 = frameserver request thread.
  Faulting instr: `rep stosb` at MediaSource!memset_repstos+0x9 (UCRT memset inlined into our DLL).
  Stack: SharedMemoryFrameSource.cpp:160 (FallbackFrame memset) ← :151 (AcquireFrame, WAIT_TIMEOUT)
  ← MediaStream.cpp:641 (DeliverNextSample) ← :220 (RequestSample) ← frameserver TP thread.
- Registers: rdi (fault addr) = 0x0000023E0BD76000, rcx = 0x2BA000 (bytes remaining), rax = 0.
  Diag log (same session, tid=30476): `Lock hr=0 bits=0000023E0B9F0000 maxLen=3686400 curLen=3686400`.
- Arithmetic: written = 0xBD76000 − 0xB9F0000 = 0x386000 (3,694,592 B); total memset = 0x386000 + 0x2BA000
  = **0x640000 = 6,553,600 B** — not 3,686,400.
- Dump memory probes: 0xB9F0000 and 0xBD74000 readable (zero), 0xBD76000+ uncommitted (`??`)
  → frameserver buffer commits exactly 3,686,400 + 8,192 B; memset faults at first uncommitted page.
- **ROOT CAUSE**: `SharedMemoryContract.h:12` `VCamFrameSize = VCamWidth * VCamStride` = 1280×5120 =
  **6,553,600**; the correct RGB32 frame is `VCamHeight * VCamStride` = 720×5120 = **3,686,400**
  (the code comment says 3686400 — formula is simply wrong). `FallbackFrame` (SharedMemoryFrameSource.cpp:160)
  `memset(pDest, 0, VCamFrameSize)` overflows the 3,686,400-byte frameserver sample buffer by 2,867,200 B
  → AV → FrameServer crash. Trigger = no producer (40 ms ready-event timeout → fallback path); with a
  producer the row-copy loop (720 rows × 5120 B, lines 141-143) stays in-bounds.
- Other users of the same constant: :145 `memcpy(m_pCache, pDest, VCamFrameSize)` = 2.86 MB OOB READ
  (producer path, latent); :157 `memcpy(pDest, m_pCache, ...)` = OOB WRITE (cached-frame fallback, latent);
  :54/:139 + ProducerTest :30/:99 section+slot geometry (self-consistent, 52.4 MB vs 29.5 MB — waste only);
  :106 m_pCache, MediaStream.cpp:574 Nv12Scratch, :588 MFCreateMemoryBuffer (over-allocated, harmless);
  MediaStream.cpp:305 MF_MT_SAMPLE_SIZE advertised as 6,553,600 (wrong metadata).
- Historical WER fault RVAs (0x1064/0x32E0/0x5F00/0x6114 → CAttrLogProxy::QI AddRef, CMediaSource::KsMethod,
  CMediaStream dtor via Release, GetEvent+0x34) mapped earlier by cdb over the installed DLL: all COM
  refcount/vtable sites — consistent with the 2.86 MB OOB write corrupting adjacent frameserver pool
  state (hypothesis; this dump proves the deterministic memset AV).
- **R2 FIX (applied)**: SharedMemoryContract.h:12 → `VCamFrameSize = VCamHeight * VCamStride;` (= 3,686,400,
  matches comment). Rebuild Release x64 → deploy C:\Program Files\VCam\MediaSource.dll → retest both
  scenarios: (a) no-producer capture (crash repro, expect 10/10 black frames, FrameServer stays up),
  (b) producer capture (expect 10/10 real frames). Then remove WER LocalDumps key (keep dump), update here.

## PRIOR SERIES (13.09.2026 ~19:40-21:30) — STALE-REGISTRATION: 2-round budget: SPENT — VERDICT: R2 retest FAILED (0/10) — its "NV12 path" crash hypothesis SUPERSEDED by the minidump series above (actual fault = OOB memset via bad VCamFrameSize, no NV12 involved)
R2 corrected retest (user-approved, 21:25:16): after first registration's holder (26028) was KILLED
21:00:41 (collateral of 180s outer-timeout tree-kill of elevated `-Wait` launch → camsvc stopped →
sensor group reaped → camera vanished; 21:05 "r2our" capture then hit Brio device[0], invalid),
re-registered via r2_add2.ps1 (non-blocking elevated, 21:24:20): FrameServer pid 24320, Registrar
pid 30640 ALIVE, sensor group 2D1C90D7... + PnP node restored, inspect count=2 (device[0]=Brio 339,
device[1]=ours 2 types). Capture `CaptureTest.exe device 1 ...r2ours` (forced index 1 = ours):
ActivateObject S_OK, currentType NV12 1280x720@30 (sub {00000016-...}), MENewStream,
RequestSample(frame 0) = 0xC00D3E9B, 0/10, exit 1. Diag delta 3016 → our DLL loaded in camsvc.
ROOT CAUSE (evidence complete): EVERY session in msrc_diag.log (13 total, 17:17→21:25:40) ends at
"Stream.DeliverNextSample before AcquireFrame" (MediaStream.cpp:624) — NO "AcquireFrame done" ever.
AcquireFrame is BOUNDED (40ms wait + FallbackFrame, SharedMemoryFrameSource.cpp:119) so the host
process died mid-call. WER Application Error 1000: svchost.exe_FrameServer 10.0.26100.8737
(0x8dd923d6) faulting module MediaSource.dll 0.0.0.0 (0x6aa6bc93), exception 0xC0000005,
at 20:58:06..20:59:51 (×9, first registration), 21:25:16 (0x5F00, our capture), 21:25:29 (0x6114),
21:25:41 (0x1064), 21:27:23 (0x32E0) → camsvc now STOPPED (did not auto-restart). Varying fault
offsets ⇒ dangling/corrupt pointer in NV12 path. SUSPECTS: (a) ConvertRgb32ToNv12 at
MediaStream.cpp:626-628 (m_pNv12Scratch / pBits); (b) allocator type mismatch — buffer maxLen=3686400
(1280×720×4 RGB32) while stream type is NV12 (1382400): InitializeSampleAllocator attributes come
from the wrong media type. Client-side 0xC00D3E9B = MF_E_MEDIA_SOURCE_WRONGSTATE from the dead
FrameServer session. Note: Brio (device[0]) ALSO fails 0/10 with 0xC00D3E9B standalone (21:05
capture) — physical-camera-side issue, separate from our crash.
R1 (no code changes): clean reset (killed stale FrameServer svchost 32160, restarted camsvc,
pnputil /scan-devices); re-registered reference (regsvr32, VCamSample elevated → pid=7896,
published 19:42:58.596).
- inspect: count=2. device[0]=ours 9C15... = **339 types** (YUY2 73 + NV12 133 + NV21 133;
  19 sizes 640x480..1920x1080 x 7 rates 30/24/20/15/10/7.5/5 — the OLD DLL's PD), name='(none)'.
  device[1]=reference C698... = 1280x960@30, 2 types.
- Reference capture 10/10 (10 BMPs in temp\opencode\r1ref) — the 18:59 "environment blocks
  everything" verdict was a STALE-FRAMESERVER artifact: new FrameServer serves reference fine
  under FFM=1. FFM=1 is NOT the blocker (no probe needed).
- Our capture 0/10 (r1our empty): ActivateObject/CreatePD/Start/MENewStream S_OK,
  RequestSample(frame 0) = 0xC00D3E9B MF_E_MEDIA_SOURCE_WRONGSTATE, exit 1.
- BOTH diag logs checked: temp\opencode\msrc_diag.log (756425 B, lastWrite 18:16:47) and
  C:\Windows\Temp\vcam_ls_load.log (47927 B, lastWrite 17:45:14; SYSTEM fallback, mirrors the
  user log — SYSTEM has FullControl on the opencode dir). ZERO entries at the ~19:55 failing
  capture → our DLL was loaded by NO process during it.
- Elevated module scan: only FrameServer svchost pid=22192 (svchost -k Camera -s FrameServer,
  session 0) holds a VCam DLL — VCamSampleSource.dll (reference). Ours: loaded by nobody.
  camsvc = svchost pid=11240 (-k osprivacy -p -s camsvc).
- SensorGroups: 9C15... published **14:55:19.883** = before Registrar fix (main.cpp 15:24:37)
  and 18:09:07 rebuild → stale entry from OLD Registrar (Lifetime_System/AllUsers/
  KSCATEGORY_VIDEO_CAMERA/1) + OLD DLL (339-type matrix). New DLL (RGB32 640x480 + NV12
  1280x720@30, 233984 B) deployed to C:\Program Files\VCam\MediaSource.dll but NEVER published.
- Enum keys 9C15.../C698... are identical generic swdevice entries (Driver \0010 vs \0011);
  SensorGroups blobs: ours 1314 B DeviceStream/464 B Attributes vs ref 1298/476.
- ROOT CAUSE: stale registration — FrameServer serves the 14:55:19 shell (old PD) for 9C15...;
  our current source is never instantiated; shell dies WRONGSTATE on RequestSample.
- R2 (code change already made+built: Registrar aligned to reference — Session/CurrentUser/
  nullptr/0 + hold arg; built 18:09:07, never run): remove stale 9C15... registration,
  restart FrameServer, run NEW `Registrar.exe add VCam hold` (elevated, NO -Wait), inspect
  (expect 2 types), ONE capture retest (device <ours> r2our).

## PRIOR SERIES (13.09.2026 ~18:50-19:04) — CONTROL EXPERIMENT: Microsoft reference VCamSample, 2-round budget: SPENT — VERDICT: REFERENCE ALSO FAILS (0/10 frames, identical signature) → Branch B: document evidence, stop [superseded by R1 above: stale-FrameServer artifact, reference works after reset]
R1 (18:52-19:00): built official VCamSample from nupkg (Release x64), deployed to `C:\Program Files\VCamRef\`
(VCamSample.exe 277504 B, VCamSampleSource.dll 521216 B), regsvr32 (rc=5 transient; registry verified
ThreadingModel=Both, no WOW6432Node copy), launched elevated. Registration params: Lifetime_Session,
Access_CurrentUser, name "VCamSample", CLSID {3CAD447D-F283-4AF4-A3B2-6F5363309F52}, category nullptr,
assocCount 0. Ours: Lifetime_System, Access_AllUsers, "VCam", {B2B674D4-9CF0-461C-BDCE-3D56FBB41356},
category KSCATEGORY_VIDEO_CAMERA, assocCount 1. Reference Activator extras: PROVIDE_ASSOCIATED=1 +
MFT_TRANSFORM_CLSID_Attribute; both set FRAMESERVER_SHARED=1; reference KsProperty/KsEvent also return
ERROR_SET_NOT_FOUND (same as ours).
- Inspect: 2 VIDCAP devices — [0]=ours (339 types, 640x480 YUY2+MJPG), [1]=reference (exactly 2 types
  @1280x960@30: NV12 {00000016-0000-0010-8000-00AA00389B71} + YUY2 {3231564E-...}); both name='(none)'
  (FriendlyName absent on both virtual cameras → name-matching impossible, index/signature only).
- Host polls (r1_ref_hosts.log, r1_ref_hosts2.log): FrameServer **svchost pid=32160 session 0** stably
  hosted `C:\Program Files\VCamRef\VCamSampleSource.dll` across both windows (18:55:34-18:56:16,
  18:58:11-18:58:47); FrameServer service Running/Manual. DLL also loaded in VCamSample.exe itself
  (in-proc, session 2).
- CAPTURE (18:59:22, `CaptureTest.exe device 1 <prefix>` = reference NV12 1280x960@30): ActivateObject S_OK,
  CreatePresentationDescriptor S_OK (stream0 = NV12 1280x960), SelectStream S_OK, Start S_OK,
  MENewStream(205) received, **RequestSample(frame 0): hr=0xC00D3E9B = MF_E_MEDIA_SOURCE_WRONGSTATE,
  0/10 frames**, Stop S_OK, exit 1.
- **IDENTICAL signature to our implementation across ALL prior series** (Start S_OK → MENewStream →
  RequestSample WRONGSTATE → 0 frames). Microsoft's own reference, which we did not write, reproduces it.
- **VERDICT: the environment blocks frames for any virtual camera** (Win11 25H2 build 26200): the
  frameserver-side shell (KS device node with fallback format, real source never attached) reproduces on
  reference code. Our code is NOT at fault for the 0-frame result. R2 = Branch B (document, stop) — no
  further attempts per budget.
- NEXT (no budget, environment-level): FrameServer never attaches ANY real MF media source to the shell
  stream for either implementation. Candidate directions: Win11 25H2 frameserver behavior, KS
  KSPROPERTYSETID_Topology handling (add-time probe dies on it for ours; reference hits ERROR_SET_NOT_FOUND
  too but somehow registers differently), OS support ticket with both DLLs as evidence.
- HARNLESS lessons: (1) `Start-Process -Verb RunAs -Wait` waits on the WHOLE elevated process tree — a
  120 s shell timeout killed VCamSample mid-experiment (camera deregistered ~18:58, caused a 1-vs-2 device
  confusion); launch without -Wait, verify liveness separately. (2) Reference app is a TaskDialog GUI;
  session-lifetime camera deregisters on app exit — re-verify Get-Process + inspect before each capture.
  (3) Elevated apps must be killed via elevated shell (plain Stop-Process fails silently).
- Final state: VCamSample.exe STOPPED (19:03:42), its camera deregistered (inspect count=1); reference
  files left in C:\Program Files\VCamRef (registered, harmless; remove via elevated `regsvr32 /u
  "C:\Program Files\VCamRef\VCamSampleSource.dll"` if wanted); our VCam UNCHANGED (registered,
  enumerable, count=1); no frame files (ref_frames/ empty); no stray processes.
- Evidence: temp\opencode\r1_ref_{deploy,launch,poll,poll2}.ps1 + r1_ref_deploy.log, r1_ref_hosts.log,
  r1_ref_hosts2.log; capture output preserved in session (18:59:22 block above).
- Consumer notes: non-RGB32 frames are saved as `.raw` (main.cpp ~604-612) so a working NV12/YUY2 camera
  would still produce artifacts; BMP only for RGB32. Solution-level
  `build\x64\Release\CaptureTest.exe` is the current binary.

## PRIOR SERIES (13.09.2026 ~17:00-18:20) — FrameServer-mode + NV12, 2-round budget: SPENT — VERDICT: FAILED (0/10 frames both rounds)
R1 fix (the only 2 allowed, user-approved): HKLM\SOFTWARE\Microsoft\Windows Media Foundation\Platform
`EnableFrameServerMode` 0x1 + CLSID InprocServer32 `ThreadingModel=Both`.
R1 (17:31-17:45): remove → deploy DLL → add. FrameServer svchost **pid=16636 (session 0)** loaded the
DLL and ran the add-time probe at 17:32:33.121 (26 ms): FinalConstruct → CreatePresentationDescriptor →
SetDefaultAllocator(shared OK) → SetMediaType id=0 → `KsEvent set={C6E13370-30AC-11D0-A18C-00A0C9118956}
(KSPROPERTYSETID_Topology) id=8 -> ERROR_SET_NOT_FOUND` → BeginGetEvent → immediate Shutdown.
Consumer (17:32:40, `CaptureTest.exe device VCam frame`): count=1, name='(none)', PD = 640x480 YUY2@30
(KS-style), Start S_OK, MENewStream, **RequestSample 0xC00D3E9B (MF_E_MEDIA_SOURCE_WRONGSTATE), 0/10 frames**.
msrc_diag: **zero source instantiations during consumer capture** — FrameServer never attached our source
to the consumer stream.
R2 (only because R1 failed): (a) MFTrace add (17:45:14): 40 clean events, no MF-API rejection;
(b) NV12 implemented: PD = 2 types (RGB32 1280x720@30 primary + NV12 1280x720@30 second), SetMediaType
accepts either, per-frame BT.601 2x2-UV conversion via scratch (shared mem is RGB32-only; byte order
R,G,B,A per ProducerTest). Build clean: `build\x64\Release\MediaSource.dll` = **233,984 bytes 18:09:07**
(NOTE: solution output is `build\x64\Release\`, NOT `src\MediaSource\build\...` which is stale).
R2 single retest (18:16-18:17): stop S_OK → remove failed 0xC00D36B2 (camera Started; harmless) →
DLL copy blocked (locked by FrameServer svchost 16636) → **killed svchost 16636** (recoverable on-demand
service) → copy OK → producer + consumer.
R2 retest result (18:17:08): **IDENTICAL to R1** — name='(none)', PD still 640x480 YUY2@30 (our RGB32/NV12
1280x720 types never appear), Start S_OK, MENewStream, RequestSample 0xC00D3E9B, 0/10 frames.
msrc_diag: no DLL activity at 18:17:08 (last entry = 18:16:47 PROCESS_DETACH of the killed svchost).
**NV12 hypothesis REFUTED** — the frameserver device is a fallback shell unrelated to our source's PD.
CONFOUNDER: R2 producer died at once — `CreateFileMappingW failed: 5` (ACCESS_DENIED) on
`Global\VCam.FrameBuffer.v1`; a lingering session-0 holder owned the Global mapping, so no frame data
existed during R2 capture. (R1 producer succeeded — its log is empty = healthy.)
ROOT CAUSE (unchanged, now confirmed from the frameserver side): the KS device node carries the DEFAULT
640x480 YUY2@30 fallback; our PD never propagates; the add-time probe dies on the Topology set
(ERROR_SET_NOT_FOUND) + immediate Shutdown; frameserver never instantiates our source for a consumer
stream; RequestSample dies in the shell with WRONGSTATE.
Final state: camera registered + enumerable (consumer sees count=1 at 18:17:08); registry:
EnableFrameServerMode=1, InprocServer32 default=`C:\Program Files\VCam\MediaSource.dll` + ThreadingModel=Both;
deployed DLL = new NV12 build (233,984 bytes 18:09:07); FrameServer svchost 16636 killed (auto-restarts on
demand); no stray ProducerTest/CaptureTest/Registrar processes.
Evidence: temp\opencode\{r2_retest_stop,remove,copy,add}.log, r2_swap_holders.log, r2_swap_done.txt,
r2_retest_capture.log, r2_retest_producer.log, msrc_diag.log.
Harness lesson: `Start-Process <bat> -Verb RunAs` silently did NOT execute (no logs at all, 3x) while
`Start-Process powershell -Verb RunAs -ArgumentList "-NoProfile","-ExecutionPolicy","Bypass","-File",<ps1> -Wait`
WORKS — use the elevated-ps1 pattern.
NEXT SERIES START POINT (if frames-through-device revisited): (1) implement KSPROPERTYSETID_Topology
(KSPROPERTY_TOPOLOGY_NODES/NodeAttributes, TopologyID 0) in IKsControl::KsProperty so the node gets our
real format; (2) set MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME in source attributes (device name is '(none)');
(3) fix producer mapping ownership (Global mapping created by a session-0 process blocks non-elevated
CreateFileMappingW with error 5 — create as SYSTEM or open-not-create in producer); (4) the add Start
callback was never observed firing across ANY series — probe separately.

## PRIOR SERIES (13.09.2026 ~12:33-12:50) — device-path frames, 2-round budget: SPENT — VERDICT: FAILED (0/10 frames both rounds)
R1 fix: consumer name-match fallback (accept first VIDCAP device when no FRIENDLY_NAME;
device carries name='(none)'). R1 (12:35): device opens, `Start` S_OK, MENewStream,
but `RequestSample` = **0xC00D3E9B = MF_E_MEDIA_SOURCE_WRONGSTATE** (mferror.h:1149) -> 0 frames.
add Start callback timed out (harness opened device ~60s after add, outside 30s window).
R2 (12:48, single retest, targeted fix = FrameServer running before open + consumer INSIDE window):
FrameServer was RUNNING (verified) and consumer opened device 10.4s after add (12:48:19.5 vs add 12:48:09.12).
Result IDENTICAL: PD still shows KS-style `major={73646976-...} sub={32595559-...} 640x480@30`
(KSMediaType_Video/KSVideoSubtype_YUY2), Start S_OK, RequestSample 0xC00D3E9B, 0 frames, exit 1.
**add Start callback TIMED OUT AGAIN (12:48:39) despite consumer opening the device inside the window.**
Diag (msrc_diag.log, byte-snapshot before R2 = 53622): ONLY Registrar in-proc probe
(pid=11516, 12:48:09.140-147). **No process (consumer, svchost -k Camera, FrameServer) ever
instantiated our source during either consumer run.** svchost -k Camera never appeared.
BOTH hypotheses refuted: H1 (FrameServer must be running) and H2 (device open fires pending callback).
Final state: camera registered + enumerable (finalstate.log count=1, 12:49:29), FrameServer RUNNING,
no stray ProducerTest/CaptureTest/Registrar processes, no BMPs.
ROOT CAUSE (device path): the virtual camera's KS device node was created with a DEFAULT
fallback format (640x480 YUY2@30) — our source's PD type (MFMediaType_Video RGB32 1280x720@30,
MediaStream.cpp ~L275) never propagated into the node — and the device source behind the node is a
shell that never attaches our media source (no SetMediaType/RequestSample ever reach us; the
`RequestSample` call dies in the shell with WRONGSTATE). In-proc add probe shows
`Src.KsProperty/KsEvent set={C6E13370-30AC-11D0-A18C-00A0C9118956} (KSPROPERTYSETID_Topology)
id=8 -> ERROR_SET_NOT_FOUND` during node construction — prime suspect for the fallback.
NEXT SERIES START POINT (if frames-through-device is revisited): fix KS topology/format
registration so the node carries our real format and the device actually instantiates our source:
(1) implement KSPROPERTYSETID_Topology (KSPROPERTY_TOPOLOGY_NODES/NodeAttributes, TopologyID 0)
in IKsControl::KsProperty, or (2) diff VCamSample's IKsControl/media-type setup (does the reference
expose a KS-compatible media type / MFVideoFormat for KS? e.g. YUY2 or MFMediaType + KS subtype
pairing) against ours. Also note the add callback NEVER observed firing across all series runs —
a consumer device-open does NOT complete `IMFVirtualCamera::Start`; probe whether the callback
needs the device opened WITHIN the same frameserver session or whether AddSourceAsync is expected
to complete on first real capture.
Evidence: temp\opencode\vcam10\{devcap10,devcap11,registrar11,frameserver11,remove11,poll10,finalstate}.log.

## PRIOR SERIES (13.09.2026 ~12:16-12:25) — device-path capture, 2-round budget: SPENT
Task: capture >=10 frames THROUGH THE DEVICE path (MFEnumDeviceSources -> ActivateObject ->
Start -> RequestSample), not CLSID-direct. R1 = build consumer + scenario; R2 = one fix.
- R1: rewrote `src\CaptureTest\main.cpp` (device mode + direct mode kept). Scenario
  (remove -> add -> producer -> consumer, logs r1_*.log in temp\opencode):
  `MFEnumDeviceSources(nullptr, ...)` = **E_INVALIDARG (0x80070057)** — NULL filter unsupported.
  Consumer exit 1, device never opened; elevated `Registrar.exe add` got Ctrl-C (0xC000013A)
  at ~26 s (console window closed by user, probably) — before its 30 s callback timeout.
- R2 (single fix): enumerate with `MFCreateAttributes` +
  `SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID)`.
  Result: `hr=0 count=1` — **our VCam IS enumerated as a system VIDCAP device**
  (sourceType {8AC3587A-4AE7-42D8-99E0-0A6013EEF90F}). BUT the device carries
  **NO MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME** (name='(none)') -> my name-substring match
  exited 2 -> device not opened -> `r2_add.log`: "Warning: Start callback timeout (30s)".
  Host poll: never saw a process with MediaSource.dll loaded (expected — no device open).
- STOPPED per 2-round rule. Final state: camera left registered (r2 add), no stray
  ProducerTest/CaptureTest/Registrar processes, no services touched.
- NEXT SERIES START POINT: in `RunDeviceMode`, match the device WITHOUT relying on
  FRIENDLY_NAME — e.g. accept the first VIDCAP device when name is (none), or read the
  VIDCAP symbolic-link attribute; then ActivateObject/IID_IMFMediaSource/Start/pump as coded.
  That is the only remaining consumer-side gap before frames flow.
- Pruned-SDK compile lessons (CaptureTest):
  - `#define INITGUID` must come BEFORE `#include <windows.h>` (guiddef.h fixes
    DEFINE_GUID to extern-only once included; mfapi.h MF_MT_* GUIDs are DEFINE_GUID).
  - PROPVARIANT VT_CLSID member is `vt.puuid` (no `clsid`/`puuidVal`).
  - `GUID_NULL` undefined — use `GUID g{};`.
  - IMFPresentationDescriptor: `GetStreamDescriptorByIndex(i, &bSel, IMFStreamDescriptor**)`
    (no GetStreamDescriptor/IMFPresentationStreamDescriptor); media types via
    `IMFStreamDescriptor::GetMediaTypeHandler` -> `IMFMediaTypeHandler::GetMediaTypeByIndex`.
  - mfidl.h device-attr GUIDs (MF_DEVSOURCE_ATTRIBUTE_*) are self-defining EXTERN_GUID.
- r2_add.exit came out empty (bat console closed before ADD_RC echo) — trust r2_add.log text.

## Task
Make `Registrar.exe add` succeed — register + start the MF virtual camera "VCam"
(MFCreateVirtualCamera + Start). Env: Windows, PowerShell 5.1, NOT elevated by default
(elevation via `Start-Process -Verb RunAs -Wait` with .bat files).

## HARD RULE
Max **5 rounds** of (change -> rebuild -> retest) to make `Registrar.exe add` succeed.
After round 5 fails, STOP and report.

**STATUS: 5/5 ROUNDS USED (RUN6..RUN10). BUDGET EXHAUSTED. STOPPED & REPORTED.**
**FINAL RESULT: `Registrar.exe add` STILL FAILS — `Failed to start camera: 0x80070005` (E_ACCESSDENIED).**
Registration itself succeeds (SensorGroups key + PnP instance created); only the async
`IMFVirtualCamera::Start` result is a failure.

## Two historical phases
- **Phase A** (earlier agent, rounds 1-5, error **0x80010105 RPC_E_SERVERFAULT**): fault was
  inside the FrameServer service process; ~0.39 s gap after ActivateObject then async fault;
  our source methods (CreatePresentationDescriptor/GetService/Start) were never called.
  Work done: tid in diag, no COM init in DllMain, SOURCE_TYPE as GUID,
  MF_VIRTUALCAMERA_PROVIDE_ASSOCIATED_CAMERA_SOURCES=0 (attr GUID {F0273718-...}).
- **Phase B** (this effort, RUN6..RUN10, error **0x80070005 E_ACCESSDENIED**): after reference-parity
  work (VCamSample), the failure mode changed to E_ACCESSDENIED at RUN9. RUN10 = final round.

## RUN10 (round 5, FINAL) — evidence
- Build: clean (only pre-existing C4996 `_vsnwprintf` in dllmain.cpp). DLL 226816 bytes, 13.09.2026 07:56.
- `registrar10.log`: `Failed to start camera: 0x80070005`. `remove` (pre-step) = 0xC00D36B2 (nothing to remove — harmless).
- Diag (msrc_diag.log, pid=9340, ~25 ms, single process = Registrar's in-proc probe):
  `DllGetClassObject(IClassFactory) -> Act.FinalConstruct -> CreateInstance(IMFActivate 7FEE9E9A) ->
  Act QI-probe batch -> GetItemType/SetItem FRIENDLY_NAME 60D0E559 (S_OK) ->
  GetUINT32 F0273718 (S_OK) -> ActivateObject(3C9B2EB9) -> Src.FinalConstruct + Stream.FinalConstruct ->
  CMediaSource QI-probe batch (incl. IMFMediaSource 1868091E S_OK, 279A808D S_OK, FA993888 S_OK) ->
  GetSourceAttributes x3 -> GetService(sid=0, riid=2032C7EF...) -> CreatePresentationDescriptor ->
  GetStreamDescriptor -> GetStreamAttributes -> GetService(sid=0, riid=B91EBFEE...) ->
  QI IKsControl 28F54685 S_OK -> **QI IInspectable AF86E2E0 S_OK x2 (the fix works; was E_NOINTERFACE in RUN9)** ->
  immediate teardown (Act.~dtor, Src.Shutdown, Stream.~dtor, Src.~dtor).
- **No `Src.Start` / `Stream.Start` / `SetMediaType` / `RequestSample` ever called. No IInspectable
  methods (GetIids/GetRuntimeClassName/GetTrustLevel) ever called.** No second (frameserver-service)
  activation appears in the diag at all.
- Registry after run (left as-is): SensorGroups\9C151270C767... present (VirtualCamera=1,
  SymbolicLinkName \\?\SWD#VCAMDEVAPI#9C151270...#{588c8d20-c0e3-4fd3-b511-8f2f692156f8}\{FCEBBA03-9D13-4C13-9940-CC84FCD132D1},
  EnableTimestamp 07:56:32.900); PnP instance HKLM\SYSTEM\CCS\Enum\SWD\VCAMDEVAPI\9C151270... = True.
  Cleanup if desired: elevated `Registrar.exe remove`.
- **FrameServer service state: STOPPED** (svchost.exe -k Camera, NT AUTHORITY\LocalService, Manual).
  No System/SCM events in the 07:55-07:58 window (no service start attempt or crash recorded).
  MediaFoundation/Operational event log does not exist on this machine.
- Note: this shell is NOT elevated; `Start-Service FrameServer` from it fails with
  "could not open service" (my permissions — not diagnostic).

## Leading remaining hypotheses (no budget left)
1. **FrameServer service not running and not auto-started** — Start's IPC to the stopped
   LocalService service may map to E_ACCESSDENIED; nothing in the system started it.
   Next probe: start FrameServer (elevated), re-run `Registrar.exe add`; also run the reference
   VCamSample Registrar on this machine to confirm it still starts (control).
2. **Security/ACL**: with MFVirtualCameraAccess_AllUsers + Lifetime_System the LocalService
   frameserver must later load the DLL from `C:\Users\Semen\source\repos\...` (user profile);
   LocalService may lack traverse/read on the profile path. Probe: copy the DLL to
   `C:\Program Files\...` (or grant LocalService read) and re-register.
3. **Residual reference divergence not visible in our diag** (something the frameserver checks
   that produces no log line on our side).

## Build fixes applied this round (all compile-clean)
- New `src\Common\SampleAllocatorControl.h` — local mirror of SDK `IMFSampleAllocatorControl`
  (MIDL_INTERFACE "DA62B958-3A38-4A97-BD27-149C640C0771") + `MFSampleAllocatorUsage`
  (UsesProvidedAllocator=0/UsesCustomAllocator=1/DoesNotAllocate=2). SDK copy is inside
  `#if WINAPI_PARTITION_PARTITION_APP` guard (mfidl.h ~21685) so desktop builds don't see it.
- `MediaSource.h` includes it; `CMediaSource` derives IMFSampleAllocatorControl.
- QI uses `__uuidof(IMFSampleAllocatorControl)` (the SDK `IID_IMFSampleAllocatorControl`
  extern is also inside the APP guard — unavailable).
- `MediaStream.h`: `m_pAllocator` is `IUnknown*` (IMFMediaAllocator absent from local SDK);
  `SetAllocator` stores the IUnknown handle (AddRef/Release) — no QI.
- `__uuidof(IKsControl)` is C2787 here -> use `IID_IKsControl` in s_iids (MediaSource.cpp, Activator.cpp).
- GetIids: `iids` is `IID**`, so `iids[i]` is `IID*` —
  `iids[i] = const_cast<IID*>(&s_iids[i]);` (all three objects).
- `WindowsCreateString` not declared by local SDK `winrt\hstring.h` (um\hstring.h doesn't exist) —
  `extern "C" HRESULT WINAPI WindowsCreateString(PCWSTR, UINT32, HSTRING*);` in each cpp
  (runtimeobject.lib already linked).
- Local SDK 10.0.26100.0 is heavily pruned: no um\wtypes.h (shared\wtypes.h only),
  cguid.h has extern-only GUID decls, IInspectable local GUID AF86E2E0-B12D-4C6A-9C5A-D7AA65101E90.

## Round history (Phase B: RUN6=r1 .. RUN10=r5)
- RUN6 (r1): baseline parity build. RUN7 (r2), RUN8 (r3): iterative parity fixes.
- RUN9 (r4): Start failed 0x80070005; only reference-vs-ours discrepancy = IInspectable
  (ours returned E_NOINTERFACE; reference S_OK).
- RUN10 (r5, FINAL): IInspectable + allocator + build fixes applied. IInspectable QI now S_OK,
  but Start still 0x80070005; teardown before any media-session call. STOP & REPORT per rule.

## GUID identifications (confirmed)
- `{F0273718-4A4D-4AC5-A15D-305EB5E90667}` = MF_VIRTUALCAMERA_PROVIDE_ASSOCIATED_CAMERA_SOURCES.
- `{60D0E559-52F8-4FA2-BBCE-ACDB34A8EC01}` = MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME.
- `{279A808D-AEC7-40C8-9C6B-A6B492C78A66}` = IID_IMFMediaSource.
- `{1868091E-AB5A-415F-A02F-5C4DD0CF901D}` = (QI'd on source, S_OK in RUN10 — IMFMediaSource-family).
- `{1C733A30-2A1C-11CE-ADE5-00AA0044773D}` = ICallFactory (telephony) — rejected, correct.
- `{334D391F-0E79-3B15-C9FF-EAC65DD07C42}` = private/undocumented — QI'd ~7x, rejected, correct.
- `{7FEE9E9A-4A89-47A6-899C-B6A53A70FB67}` = IID_IMFActivate (S_OK).
- `{28F54685-06FD-11D2-B27A-00A0C9223196}` = local IID_IKsControl (S_OK in RUN10).
- `{AF86E2E0-B12D-4C6A-9C5A-D7AA65101E90}` = local IInspectable (S_OK in RUN10; canonical
  alias AF86E2E0-589B-42B4-823F-0904203D4E8C kept as kIID_IInspectable_Canonical in GUIDs.h).
- `{94EA2B94-A7C1-400B-8A6E-80F10AABFFF8}` = canonical IID_IMFAttributes (local = 2cd2d921-...).
- `{3C9B2EB9-86D5-4514-A394-F56664F9F0D8}` = ActivateObject riid (IMFMediaSourceEx-family).
- `{FA993888-4383-415A-A930-DD472A8CF6F7}` = QI'd x2 on source, S_OK.
- `{B2B674D4-9CF0-461C-BDCE-3D56FBB41356}` = our virtual-camera CLSID.

## Build (Release|x64) — only pre-existing C4996 warning
`& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" "C:\Users\Semen\source\repos\VirtualCameraMediaSource\VirtualCameraMediaSource.sln" /p:Configuration=Release /p:Platform=x64 /m /v:minimal /nologo`
MSBuild 18.10.1, v145, NTDDI_VERSION=0x0A000004, _WIN32_WINNT=0x0A00.
vcxproj includes: `$(ProjectDir);$(ProjectDir)..\Common;C:\Program Files (x86)\Windows Kits\10\Include\10.0.26100.0;%(AdditionalIncludeDirectories)`;
libs: mfplat.lib;mf.lib;mfsensorgroup.lib;runtimeobject.lib;ole32.lib.

## Key files
- src\Common\SampleAllocatorControl.h — local IMFSampleAllocatorControl mirror (this round).
- src\Common\GUIDs.h — canonical aliases + KSPROPS.
- src\MediaSource\MediaSource.{h,cpp} — CMediaSource (IMFMediaSource/Ex/2 + IMFGetService +
  IMFRealTimeClientEx + IKsControl + IInspectable + IMFSampleAllocatorControl).
- src\MediaSource\MediaStream.{h,cpp} — CMediaStream (IInspectable; m_pAllocator IUnknown*).
- src\MediaSource\Activator.{h,cpp} — CActivator IMFActivate (IInspectable; ActivateObject).
- src\MediaSource\dllmain.cpp — VCamDiagLog/VCamDiagGuid -> msrc_diag.log; DllRegisterServer.
- src\Registrar\main.cpp — MFCreateVirtualCamera(SoftwareCameraSource, System, AllUsers,
  "VCam", {B2B674D4-...}, {KSCATEGORY_VIDEO_CAMERA},1); async Start; 30 s callback wait.
- Reference: C:\Users\Semen\AppData\Local\Temp\opencode\VCamSample\ (VCamSampleSource etc.).

## Test wrapper pattern (elevated)
`C:\Users\Semen\AppData\Local\Temp\opencode\vcamNN_test_add.bat`:
cd build\x64\Release; append RUN marker to msrc_diag.log; `Registrar.exe remove` (log),
`Registrar.exe add` (stdout/err logs); echo %ERRORLEVEL% to .exit.
Run: `Start-Process -FilePath <bat> -Verb RunAs -Wait -PassThru` (UAC user-approved).
PRE-RUN: kill lingering Registrar; clear msrc_diag.log.
NOTE: the bat must have CRLF line endings — LF-only bats make `echo %ERRORLEVEL%`
expand broken (cmd emits the "ECHO is off" state line into the .exit file).
Diag file is OEM (cp866) bytes; decode with `[Text.Encoding]::GetEncoding(866)`.
Console output of Registrar is also cp866.

## On success (exit 0) — verify, do NOT unregister
- Bounded registry: `Get-ChildItem HKLM:\SOFTWARE\Microsoft -Recurse -Depth 3` (VCam/CLSID) +
  `HKLM:\SOFTWARE\Microsoft\Windows Media Foundation`.
- `Get-PnpDevice | Where-Object { $_.FriendlyName -match 'VCam|virtual camera|Media Foundation' }`.
- Kill lingering Registrar after 120s.
