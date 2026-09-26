# vcam-camera-core (subagent A — ProducerCore) — источник «camera»

Задача: этап A из плана `vcam-camera.memory.md` — тип `L"camera"` ТОЛЬКО в ProducerCore.
Ветка: feature/vcam-camera-source. Память вышестоящей: vcam-camera.memory.md +
vcam-producer.memory.md + vcam-producer-core.memory.md. Лимит 2 раунда.

## Статус
- **DONE** (в рамках раунда: 3 сборки решения exit 0; smoke FAILS=0 = 35 PASS / 0 FAIL / 0 SKIP).
- Рейл-ган раунда 1: (1) enumerate пустые атрибуты → фикс ключа symlink + VT_LPWSTR;
  (2) MFCreateSourceReaderFromURL → 0x80070002 → переход на ActivateObject +
  MFCreateSourceReaderFromMediaSource (диагностические тесты во временной папке).

## Созданные файлы (включены в ProducerCore.vcxproj)
- `src/ProducerCore/CameraDevices.h/.cpp`:
  - `struct CameraDeviceInfo { std::wstring id; std::wstring name; }`.
  - `EnumerateCameraDevices()` — MFEnumDeviceSources(VIDCAP), пусто при ошибке;
    **MFStartup/MFShutdown парно ВНУТРИ функции** (под статическим мьютексом) +
    CoInitializeEx/CoUninitialize внутри (COM не требуется снаружи) — повторные
    вызовы безопасны.
  - `OpenCameraActivate(id, name, outInfo)` — то же + матчинг (точный id i-cmp →
    точное имя → подстрока имени i-cmp), возвращает **IMFActivate с передачей
    владения** (caller Release) или nullptr; используется CameraSource::Open.
- `src/ProducerCore/CameraSource.h/.cpp` — `class CameraSource : IFrameSource`,
  Name()=L"camera":
  - Open: reuse (open_ && !failed_ && cfg==cfg_) → Close → оба пустых →
    «камера не выбрана» → CoInitializeEx+MFStartup (парные в Close) →
    OpenCameraActivate (иначе «камера не найдена: …») → OpenCameraReader →
    кэш capW*capH*4 → stopEvent → поток (CreateThread).
  - OpenCameraReader: перебор passes {videoProcessing+advanced → videoProcessing →
    plain} (advanced под `#ifdef MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING`):
    `ActivateObject(IID_IMFMediaSource)` → `MFCreateSourceReaderFromMediaSource`
    → ConfigureCameraReader. Владение: reader_ + mediaSrc_ (Close: reader->Release,
    mediaSrc->Shutdown+Release).
  - ConfigureCameraReader: первый видеопоток → SetStreamSelection → попытка
    RGB32 1280x720@30 (MF_MT_FRAME_RATE 30/1) → без fps → перебор нативных
    типов, отсортированных по близости к 1280x720 (aspectDiff*4 + area/100):
    сначала нативные уже RGB32 своим размером, затем RGB32 поверх нативных
    размеров (NV12/YUY2/MJPG конвертирует video processing) → фиксация
    capW/capH/stride из GetCurrentMediaType; выход обязан быть RGB32/ARGB32.
  - Поток CaptureLoop: ReadSample → (ConvertToContiguousBuffer → построчно в кэш
    под mutex, RowPtr учитывает отрицательный stride → top-down) ; счётчик молчания
    90 пустых/ошибочных итераций или MF_E_SHUTDOWN → failed_ + failReason_ → выход.
  - Render: под mutex; failed_/нет кадра → false с причиной; 1280x720 → построчный
    memcpy с учётом stride; иначе letterbox-fit билинейный (16.16 fixed-point,
    паттерн VideoFileSource) с произвольным dst stride.
  - Close: SetEvent → WaitForSingleObject(thread, 3000) → при успехе освобождение
    всего (идемпотентно, из деструктора); **при таймауте — ранний return без
    освобождения reader/MF (иначе UAF в зависшем потоке)**.

## Правки существующих
- `src/Common/ProducerApi.h`: `SourceConfig.camName` + operator== + комменты
  (типы L"static"|L"video"|L"camera", путь = id для camera).
- `src/ProducerCore/Settings.h`: `CameraSection{id,name}`, поле `cam`, operator==,
  коммент sourceType/Load.
- `src/ProducerCore/Settings.cpp`: ParseNewSchema читает секцию `camera` (id/name);
  Load детект новой схемы += FindObjectRange("camera"); Serialize пишет
  `"camera": { "id": …, "name": … },` перед `"autostart"`; ToSourceConfig при
  L"camera" → path=cam.id, camName=cam.name (scaleMode/crop остаются пустыми).
  Legacy-миграция не тронута.
- `SourceFactory.cpp`: L"camera" → make_unique<CameraSource>.
- `ProducerCore.vcxproj`: +4 записи (2 ClCompile, 2 ClInclude).

## ФАКТЫ (важно для CLI/UI/e2e)
- **Спека неточна в двух местах (проверено диагностикой на этом стенде)**:
  1. `MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID` ({8AC3587A-…}) — это
     ЗНАЧЕНИЕ типа источника, а не ключ symlink: GetItem по нему даёт
     MF_E_ATTRIBUTENOTFOUND. Сирлинк лежит в
     `MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK` ({58F0AAD8-…}).
     `EnumerateCameraDevices().id` = именно этот сирлинк (fallback на VIDCAP_GUID).
  2. `MFCreateSourceReaderFromURL(symlink)` → **0x80070002** (и с `\global`, и
     без). Рабочий путь: `IMFActivate::ActivateObject(IMFMediaSource)` +
     `MFCreateSourceReaderFromMediaSource`. Спека-«FromURL» — эмпирически мёртв.
  3. Строковые атрибуты activate приходят как **VT_LPWSTR** (vt=31), не VT_BSTR —
     CaptureTest проверяет VT_BSTR, т.е. его `link=` вероятно был «(none)».
- Камера сейчас: **count=1** (раньше в CaptureTest было 2) — «Brio 90»,
  id=`\\?\usb#vid_046d&pid_0949&mi_00#6&466a1bd&0&0000#{e5323777-…}\global`.
- Smoke (Temp\opencode\camera_smoke.cpp, FAILS=0): enumerate 1; Open(id) → true;
  Render → true (первый кадр < 10 с), буфер не весь ноль; reuse Open → true;
  Close ×2 идемпотентен; Render после Close → false; Open после Close → true;
  подстрока имени «Brio» → true; пустые id+name → false «камера не выбрана»;
  несуществующий → false «камера не найдена: …».
- Settings: Load json с camera → поля совпали; Serialize round-trip equal;
  Serialize содержит `"camera"`; legacy json без camera → cam пустой, старое
  поведение не сломано (19/19 settings-чеков); ToSourceConfig(s, camera) корректен.
- Реальный `%APPDATA%\VCam\settings.json` (409 B) — только читался, до/после
  байты совпали. Никаких Save в реальный путь тест не делал.
- Сборка решения Release|x64: **exit 0** (3 прогона; MSB3027 не встречался).

## Риски
- Зависший навсегда ReadSample: Close с таймаутом 3 с НЕ освобождает
  reader/mediaSrc/MF (иначе UAF) → утечка до процесса; деструктор при этом
  формально оставляет живой поток на уничтоженный `this` (после выхода из цикла
  поток только CoUninitialize). Вероятность мала; фиксируется логом
  OutputDebugString [ProducerCore:camera].
- Счётчик молчания ловит возвраты из ReadSample; реально «зависший» вызов он не
  видит (только таймаут Close).
- MFStartup парность: CameraSource держит +1 весь период Open…Close; enumerate
  делает свой +1/-1 внутри — безопасно. После Close всех потребителей MF
  выгружается (штатно).
- Enumerate/Open под мьютексом g_enumMutex — последовательные обращения ок;
  параллельный Open из разных потоков — не тестировался (по спеке Open из одного
  потока хоста).
- Веб-камера отдаёт кадры не мгновенно: host Phase::Switch даёт ~5 с окно —
  Render=true обычно < 1–2 с (проверено).
- Камера count=1 (было 2): второй устройства сейчас нет — не «починено» подгонкой.

## Запрос
- Не требуется. Следующим: subagent B (CLI + e2e), subagent C (UI).
