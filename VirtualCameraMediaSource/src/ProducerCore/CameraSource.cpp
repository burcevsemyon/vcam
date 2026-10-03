#include "CameraSource.h"

#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <propidl.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <cwctype>

#include "CameraDevices.h"
#include "FrameCopy.h"
#include "ImageLayout.h"
#include "SharedMemoryContract.h"

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mf.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")

using vcam::RowPtr;
using vcam::LetterboxBilinear;

namespace {

const UINT32 kFps = 30;             // fps фиксированный (утверждённое решение)
const int kMaxSilent = 90;          // ~3 с молчания камеры -> failed_
const DWORD kThreadJoinMs = 3000;

void LogCamera(const std::wstring& msg)
{
    OutputDebugStringW((L"[ProducerCore:camera] " + msg + L"\n").c_str());
}

std::wstring HrHex(HRESULT hr)
{
    wchar_t buf[16];
    swprintf(buf, 16, L"0x%08X", (unsigned)hr);
    return std::wstring(buf);
}

std::wstring DescribeTarget(const SourceConfig& cfg)
{
    if (!cfg.camName.empty() && !cfg.path.empty())
        return cfg.camName + L" (" + cfg.path + L")";
    if (!cfg.camName.empty()) return cfg.camName;
    return cfg.path;
}

struct CamNativeType {
    GUID sub = GUID_NULL;
    UINT32 w = 0;
    UINT32 h = 0;
};

// Классификация субтипа для NV12-first порядка. Выходной субтип — только
// NV12 (натив, без конвертера) либо RGB32 (натив или поверх через video
// processing); MJPG выходом НЕ бывает. ARGB32 — тот же layout, что RGB32.
vcam::camsource::CamSub ClassifySub(const GUID& sub)
{
    if (sub == MFVideoFormat_NV12) return vcam::camsource::CamSub::NV12;
    if (sub == MFVideoFormat_RGB32 || sub == MFVideoFormat_ARGB32)
        return vcam::camsource::CamSub::RGB32;
    if (sub == MFVideoFormat_MJPG) return vcam::camsource::CamSub::Mjpg;
    return vcam::camsource::CamSub::Other;
}

// Переговоры: Phase A — НАТИВ NV12 целевого размера в score-порядке
// (выходной субтип NV12, без конвертера); Phase B — старый RGB32-путь 1:1
// (score-порядок, RGB32-натив напрямую, остальное — RGB32 поверх через video
// processing; размеры MJPG оставлены как в старом пути — выход всё равно
// RGB32, своего JPEG-декодера у нас нет, конвертер внутри SourceReader);
// Phase C — запасные штатные 1280x720 (совместимость).
// MJPG выходным субтипом НЕ бывает (только NV12/RGB32); порядок Phase B
// без NV12-кандидатов = старому порядку побайтово (та же stable_sort +
// want-логика). Score/TargetHeight — из vcam::camsource (header).
bool ConfigureCameraReader(IMFSourceReader* rdr, UINT32 targetH, DWORD& outStream, UINT32& outW,
                           UINT32& outH, LONG& outStride, GUID& outSub, std::wstring& err)
{
    DWORD vid = MAXDWORD;
    for (DWORD i = 0; i < 128; i++) {
        ATL::CComPtr<IMFMediaType> mt;
        if (FAILED(rdr->GetNativeMediaType(i, 0, &mt)) || !mt) break;
        GUID maj = GUID_NULL;
        mt->GetMajorType(&maj);
        if (maj == MFMediaType_Video) { vid = i; break; }
    }
    if (vid == MAXDWORD) { err = L"нет видеопотока"; return false; }

    rdr->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE);
    rdr->SetStreamSelection(vid, TRUE);

    std::vector<CamNativeType> natives;
    for (DWORD i = 0;; i++) {
        ATL::CComPtr<IMFMediaType> mt;
        if (FAILED(rdr->GetNativeMediaType(vid, i, &mt)) || !mt) break;
        CamNativeType n;
        GUID maj = GUID_NULL;
        mt->GetGUID(MF_MT_MAJOR_TYPE, &maj);
        mt->GetGUID(MF_MT_SUBTYPE, &n.sub);
        MFGetAttributeSize(mt, MF_MT_FRAME_SIZE, &n.w, &n.h);
        if (maj == MFMediaType_Video && n.w && n.h) natives.emplace_back(n);
    }

    auto trySet = [&](const GUID& sub, UINT32 w, UINT32 h, bool fps) -> HRESULT {
        ATL::CComPtr<IMFMediaType> p;
        if (FAILED(MFCreateMediaType(&p)) || !p) return E_OUTOFMEMORY;
        p->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        p->SetGUID(MF_MT_SUBTYPE, sub);
        p->SetUINT64(MF_MT_FRAME_SIZE, ((UINT64)w << 32) | (UINT64)h);
        if (fps)
            p->SetUINT64(MF_MT_FRAME_RATE, ((UINT64)kFps << 32) | 1ULL);
        p->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        return rdr->SetCurrentMediaType(vid, nullptr, p);
    };

    HRESULT hr = E_FAIL;
    if (!natives.empty()) {
        // Score-порядок один для обеих фаз (стабильный; равные — входным
        // порядком драйвера). Делим с .h-тестами через те же helpers.
        std::vector<vcam::camsource::CamTry> tries;
        tries.reserve(natives.size());
        for (const CamNativeType& n : natives)
            tries.push_back({ ClassifySub(n.sub), n.w, n.h });
        const std::vector<size_t> sorted =
            vcam::camsource::SortIndicesByScore(tries, targetH);
        const std::vector<size_t> nv12 =
            vcam::camsource::Nv12IndicesInOrder(tries, sorted);
        // Phase A: NV12-натив (без конвертера). Один размер важнее субтипа
        // внутри NV12-класса — порядок уже score.
        for (size_t k : nv12) {
            const CamNativeType& n = natives[k];
            hr = trySet(MFVideoFormat_NV12, n.w, n.h, true);
            if (FAILED(hr)) hr = trySet(MFVideoFormat_NV12, n.w, n.h, false);
            if (SUCCEEDED(hr)) break;
        }
        // Phase B: старый RGB32-путь 1:1 (свой размер важнее субтипа).
        // RGB32-натив — напрямую, остальное (NV12/YUY2/MJPG-размеры) — RGB32
        // поверх через video processing. Раньше любой RGB32 (хоть 640x480)
        // побеждал крупный MJPG и конвертер не получал шанса — Brio застревала
        // на 640x480 (теперь Phase A забирает NV12-1080p раньше).
        if (FAILED(hr)) {
            for (size_t k : sorted) {
                const CamNativeType& n = natives[k];
                const GUID& want = (n.sub == MFVideoFormat_RGB32)
                    ? n.sub : MFVideoFormat_RGB32;
                hr = trySet(want, n.w, n.h, true);
                if (FAILED(hr)) hr = trySet(want, n.w, n.h, false);
                if (SUCCEEDED(hr)) break;
            }
        }
    }
    // 3) запасные штатные 1280x720 (совместимость)
    if (FAILED(hr)) hr = trySet(MFVideoFormat_RGB32, vcam::VCamWidth, vcam::VCamHeight, true);
    if (FAILED(hr)) hr = trySet(MFVideoFormat_RGB32, vcam::VCamWidth, vcam::VCamHeight, false);
    if (FAILED(hr)) { err = L"камера не даёт кадр (NV12/RGB32): " + HrHex(hr); return false; }

    ATL::CComPtr<IMFMediaType> cur;
    HRESULT hc = rdr->GetCurrentMediaType(vid, &cur);
    if (FAILED(hc) || !cur) { err = L"GetCurrentMediaType failed: " + HrHex(hc); return false; }
    GUID sub = GUID_NULL;
    cur->GetGUID(MF_MT_SUBTYPE, &sub);
    UINT64 fs = 0;
    if (FAILED(cur->GetUINT64(MF_MT_FRAME_SIZE, &fs))) fs = 0;
    LONG stride = 0;
    if (FAILED(cur->GetUINT32(MF_MT_DEFAULT_STRIDE, (UINT32*)&stride))) stride = 0;
    cur = nullptr;

    UINT32 w = (UINT32)(fs >> 32);
    UINT32 h = (UINT32)(fs & 0xFFFFFFFFu);
    if (w == 0 || h == 0) { err = L"нулевой размер кадра на выходе"; return false; }
    const bool isNv12 = (sub == MFVideoFormat_NV12);
    if (!isNv12 && sub != MFVideoFormat_RGB32 && sub != MFVideoFormat_ARGB32) {
        err = L"выход не NV12/RGB32 (субтип без поддержки)";
        return false;
    }
    if (stride == 0) stride = isNv12 ? (LONG)w : (LONG)(w * 4);

    outStream = vid;
    outW = w;
    outH = h;
    outStride = stride;
    outSub = sub;
    return true;
}

// Перебор режимов ридера: video processing + advanced (Win10+, если атрибут
// поддерживается) -> video processing -> plain. Успех = reader создался И
// выход RGB32 согласован.
// Путь открытия: IMFActivate::ActivateObject -> MFCreateSourceReaderFromMediaSource.
// MFCreateSourceReaderFromURL(symlink) на этом стенде даёт 0x80070002
// (ERROR_FILE_NOT_FOUND) — диагностика, спека неточна.
bool OpenCameraReader(IMFActivate* act, UINT32 targetH, ATL::CComPtr<IMFSourceReader>& out,
                      ATL::CComPtr<IMFMediaSource>& outSrc,
                      DWORD& outStream, UINT32& outW, UINT32& outH, LONG& outStride,
                      GUID& outSub, std::wstring& err)
{
    struct Pass { bool adv; bool vp; const wchar_t* name; };
    const Pass passes[] = {
        { true,  true, L"videoProcessing+advanced" },
        { false, true, L"videoProcessing" },
        { false, false, L"plain" },
    };

    HRESULT lastHr = E_FAIL;
    std::wstring lastErr;
    for (const Pass& p : passes) {
        ATL::CComPtr<IMFMediaSource> msrc;
        HRESULT hr = act->ActivateObject(IID_IMFMediaSource, (void**)&msrc);
        if (FAILED(hr) || !msrc) {
            lastHr = hr;
            lastErr = L"ActivateObject failed: " + HrHex(hr);
            continue;
        }

        ATL::CComPtr<IMFAttributes> attr;
        if (p.vp || p.adv) {
            if (SUCCEEDED(MFCreateAttributes(&attr, 2)) && attr) {
                if (p.vp) attr->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
#ifdef MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING
                if (p.adv)
                    attr->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);
#endif
            }
        }
        ATL::CComPtr<IMFSourceReader> rdr;
        hr = MFCreateSourceReaderFromMediaSource(msrc, attr, &rdr);
        attr = nullptr;
        if (FAILED(hr) || !rdr) {
            lastHr = hr;
            lastErr = L"MFCreateSourceReaderFromMediaSource failed: " + HrHex(hr);
            msrc->Shutdown();
            msrc = nullptr;
            continue;
        }

        DWORD st = 0;
        UINT32 w = 0, h = 0;
        LONG stride = 0;
        GUID sub = GUID_NULL;
        std::wstring cerr;
        if (ConfigureCameraReader(rdr, targetH, st, w, h, stride, sub, cerr)) {
            out = std::move(rdr);
            outSrc = std::move(msrc);
            outStream = st;
            outW = w;
            outH = h;
            outStride = stride;
            outSub = sub;
            LogCamera(std::wstring(L"reader mode: ") + p.name);
            return true;
        }
        lastErr = cerr;
        rdr = nullptr;
        msrc->Shutdown();
        msrc = nullptr;
        LogCamera(std::wstring(L"reader mode ") + p.name + L" failed: " + cerr);
    }
    err = lastErr.empty() ? (L"cannot open camera: " + HrHex(lastHr)) : lastErr;
    return false;
}

} // namespace

CameraSource::CameraSource() = default;

CameraSource::~CameraSource()
{
    // Поток завис намертво: дожидаемся бесконечно, иначе члены (mutex_, reader_)
    // будут удалены под живым CaptureLoop — UB.
    if (!Shutdown(kThreadJoinMs))
        Shutdown(INFINITE);
}

void CameraSource::SetFailed(const std::wstring& reason)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        failed_ = true;
        failReason_ = reason;
        hasFrame_ = false;
    }
    LogCamera(L"failed: " + reason);
}

bool CameraSource::Open(const SourceConfig& cfg, std::wstring& err)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (open_ && !failed_ && cfg == cfg_) return true; // reuse
    }
    // Поток обязан остановиться: иначе Open перезаписал бы reader_/stopEvent_
    // под живым CaptureLoop, а cache_.resize — под писателем в cache_.
    if (!Shutdown(kThreadJoinMs)) {
        err = L"previous capture thread is still running";
        return false;
    }

    if (cfg.path.empty() && cfg.camName.empty()) {
        err = L"камера не выбрана"; // пустая секция camera -> NO SIGNAL до выбора
        return false;
    }

    HRESULT hrCo = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    comUp_ = (hrCo == S_OK); // RPC_E_CHANGED_MODE — COM уже инициализирован иначе

    HRESULT hr = MFStartup(MF_VERSION, MFSTARTUP_FULL);
    if (FAILED(hr)) {
        err = L"MFStartup failed: " + HrHex(hr);
        if (comUp_) { CoUninitialize(); comUp_ = false; }
        return false;
    }
    mfUp_ = true;

    // Матчинг id -> точное имя -> подстрока имени (внутри enumerate).
    CameraDeviceInfo devInfo;
    ATL::CComPtr<IMFActivate> act;
    act.Attach(OpenCameraActivate(cfg.path, cfg.camName, devInfo));
    if (!act) {
        err = L"камера не найдена: " + DescribeTarget(cfg);
        Close(); // парный MFShutdown/CoUninitialize
        return false;
    }
    if (devInfo.id.empty()) {
        act = nullptr;
        err = L"камера без symlink: " + DescribeTarget(cfg);
        Close();
        return false;
    }

    ATL::CComPtr<IMFSourceReader> rdr;
    ATL::CComPtr<IMFMediaSource> msrc;
    DWORD stream = 0;
    UINT32 w = 0, h = 0;
    LONG stride = 0;
    GUID outSub = GUID_NULL;
    const UINT32 captureH = vcam::camsource::CaptureTargetHeightFor(cfg.capture);
    bool readerOk = OpenCameraReader(act, captureH, rdr, msrc, stream, w, h, stride,
                                     outSub, err);
    act = nullptr;
    if (!readerOk) {
        Close(); // парный MFShutdown/CoUninitialize
        return false;
    }
    mediaSrc_ = std::move(msrc);
    reader_ = std::move(rdr);
    streamIndex_ = stream;
    capW_ = w;
    capH_ = h;
    capStride_ = stride;
    capSub_ = outSub;
    capIsNv12_ = (outSub == MFVideoFormat_NV12);

    try {
        // под mutex_: CaptureLoop пишет cache_ под этим же локом
        std::lock_guard<std::mutex> lock(mutex_);
        cache_.resize((size_t)w * h * 4);
    } catch (...) {
        err = L"out of memory";
        Close(); // парный MFShutdown/CoUninitialize + освобождение reader/mediaSrc
        return false;
    }

    stopEvent_.Attach(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!stopEvent_) {
        err = L"CreateEventW failed: " + std::to_wstring(GetLastError());
        Close();
        return false;
    }

    cfg_ = cfg;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        hasFrame_ = false;
        failed_ = false;
        failReason_.clear();
        open_ = true;
    }

    thread_.Attach(CreateThread(nullptr, 0, ThreadProc, this, 0, nullptr));
    if (!thread_) {
        err = L"CreateThread failed: " + std::to_wstring(GetLastError());
        {
            std::lock_guard<std::mutex> lock(mutex_);
            open_ = false;
        }
        Close(); // парный MFShutdown/CoUninitialize + освобождение reader/mediaSrc
        return false;
    }

    LogCamera(L"opened: " + DescribeTarget(cfg) + L" (" + std::to_wstring(capW_) + L"x" +
              std::to_wstring(capH_) + L", capture=" +
              (captureH == 720 ? L"720p" : captureH == 1080 ? L"1080p" : L"max") +
              L", out=" + (capIsNv12_ ? L"NV12" : L"RGB32") + L")");
    // Контролы — QI с нашего же источника (без второго ActivateObject);
    // pipe-сервер для виртуалки — best effort: BUSY/ошибка не роняют источник.
    controls_.Attach(mediaSrc_);
    HRESULT hrCtl = controlServer_.Start(&controls_);
    if (FAILED(hrCtl))
        LogCamera(L"control server not started: " + HrHex(hrCtl));
    return true;
}

bool CameraSource::Render(uint8_t* bgrx, int stride, std::wstring& err)
{
    if (!bgrx || stride < (int)vcam::VCamStride) {
        err = L"invalid target buffer";
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (!open_) {
        err = L"camera source is not open";
        return false;
    }
    if (failed_) {
        err = failReason_;
        return false;
    }
    if (!hasFrame_ || cache_.empty()) {
        err = L"нет первого кадра с камеры";
        return false;
    }

    if (capW_ == vcam::VCamWidth && capH_ == vcam::VCamHeight) {
        vcam::CopyFrameRowwise(bgrx, (size_t)stride, cache_.data(), vcam::VCamStride,
                               vcam::VCamWidth, vcam::VCamHeight, vcam::VCamPixelSize);
    } else if (stride != (int)vcam::VCamStride ||
               // MFT пишет packed 1280x720 stride 5120; чужой stride — только CPU.
               !mftScaler_.Scale(cache_.data(), capW_, capH_, (LONG)(capW_ * 4), bgrx)) {
        LetterboxBilinear(cache_.data(), capW_, capH_, (LONG)(capW_ * 4), bgrx, stride);
    }
    return true;
}

bool CameraSource::NativeSize(uint32_t& w, uint32_t& h)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!open_ || failed_ || capW_ == 0 || capH_ == 0) {
        w = vcam::VCamWidth;
        h = vcam::VCamHeight;
        return false;
    }
    w = capW_;
    h = capH_;
    return true;
}

bool CameraSource::Render(uint8_t* dst, int stride, uint32_t w, uint32_t h,
                          std::wstring& err)
{
    if (!dst || w == 0 || h == 0 || w > vcam::VCamNativeCapW || h > vcam::VCamNativeCapH ||
        stride < (int)(w * 4)) {
        err = L"invalid target buffer";
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (!open_) {
        err = L"camera source is not open";
        return false;
    }
    if (failed_) {
        err = failReason_;
        return false;
    }
    if (!hasFrame_ || cache_.empty()) {
        err = L"нет первого кадра с камеры";
        return false;
    }

    if (w == capW_ && h == capH_) {
        vcam::CopyFrameRowwise(dst, (size_t)stride, cache_.data(), (size_t)capW_ * 4,
                               capW_, capH_, vcam::VCamPixelSize);
    } else if (w == vcam::VCamWidth && h == vcam::VCamHeight) {
        // Та же логика, что в legacy Render: MFT пишет packed 1280x720 stride
        // 5120 (уже записал при успехе Scale); чужой stride — только CPU.
        if (stride != (int)vcam::VCamStride ||
            !mftScaler_.Scale(cache_.data(), capW_, capH_, (LONG)(capW_ * 4), dst)) {
            LetterboxBilinear(cache_.data(), capW_, capH_, (LONG)(capW_ * 4), dst, stride);
        }
    } else {
        vcam::LetterboxBilinearEx(cache_.data(), capW_, capH_, (LONG)(capW_ * 4),
                                  dst, w, h, (LONG)stride);
    }
    return true;
}

void CameraSource::Close()
{
    Shutdown(kThreadJoinMs);
}

bool CameraSource::Shutdown(DWORD timeoutMs)
{
    if (stopEvent_) SetEvent(stopEvent_);
    if (thread_) {
        if (WaitForSingleObject(thread_, timeoutMs) != WAIT_OBJECT_0) {
            // Поток завис в ReadSample: не освобождаем reader/MF (иначе UAF в
            // потоке); деструктор повторит с INFINITE.
            LogCamera(L"capture thread did not stop in " + std::to_wstring(timeoutMs) +
                      L" ms (reader отложен)");
            return false;
        }
        thread_.Close();
    }

    stopEvent_.Close();
    // Сервер управления — до освобождения источника: потоки pipe используют
    // только IAM-указатели controls_, reader/mediaSrc им не нужны.
    controlServer_.Stop();
    controls_.Detach();
    reader_ = nullptr; // release before mediaSrc_/MFShutdown (order from 884a785)
    if (mediaSrc_) mediaSrc_->Shutdown();
    mediaSrc_ = nullptr;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        open_ = false;
        hasFrame_ = false;
        failed_ = false;
        failReason_.clear();
    }
    cache_.clear();
    cache_.shrink_to_fit();
    capW_ = capH_ = 0;
    capStride_ = 0;
    capSub_ = GUID_NULL;
    capIsNv12_ = false;
    streamIndex_ = 0;
    cfg_ = SourceConfig();

    // Скейлер — ДО MFShutdown, пока платформа жива (его MFT нельзя отпускать
    // после MFShutdown/teardown — AV на выходе; см. VideoProcessorScaler).
    // Безвредно и на холодном пути (Shutdown до первого Open).
    mftScaler_.Shutdown();

    if (mfUp_) { MFShutdown(); mfUp_ = false; } // парный MFStartup в Open
    if (comUp_) { CoUninitialize(); comUp_ = false; }
    return true;
}

DWORD WINAPI CameraSource::ThreadProc(LPVOID self)
{
    static_cast<CameraSource*>(self)->CaptureLoop();
    return 0;
}

// Тик NATIVEMEDIATYPECHANGED (нулевой сэмпл с 0x100): НЕ молчание и НЕ смерть.
// Перечитываем GetNativeMediaType(0) + GetCurrentMediaType; при расхождении
// текущего типа с cap — адаптируемся (размеры/субтип/stride + resize кэша,
// hasFrame_=false до первого кадра нового формата) и логируем. failed_ НЕ
// выставляется; failed — только по kMaxSilent настоящих пустот/ошибок.
void CameraSource::OnNativeTypeChangedTick(IMFSourceReader* rdr)
{
    if (!rdr) {
        LogCamera(L"native-type tick: no reader");
        return;
    }
    ATL::CComPtr<IMFMediaType> cur;
    HRESULT hc = rdr->GetCurrentMediaType(streamIndex_, &cur);
    GUID curSub = GUID_NULL;
    UINT32 cw = 0, ch = 0;
    LONG cstride = 0;
    if (SUCCEEDED(hc) && cur) {
        cur->GetGUID(MF_MT_SUBTYPE, &curSub);
        UINT64 fs = 0;
        if (SUCCEEDED(cur->GetUINT64(MF_MT_FRAME_SIZE, &fs))) {
            cw = (UINT32)(fs >> 32);
            ch = (UINT32)(fs & 0xFFFFFFFFu);
        }
        if (FAILED(cur->GetUINT32(MF_MT_DEFAULT_STRIDE, (UINT32*)&cstride)))
            cstride = 0;
        if (cstride == 0)
            cstride = (curSub == MFVideoFormat_NV12) ? (LONG)cw : (LONG)(cw * 4);
    }
    cur = nullptr;

    ATL::CComPtr<IMFMediaType> nat;
    GUID natSub = GUID_NULL;
    UINT32 nw = 0, nh = 0;
    if (SUCCEEDED(rdr->GetNativeMediaType(streamIndex_, 0, &nat)) && nat) {
        nat->GetGUID(MF_MT_SUBTYPE, &natSub);
        MFGetAttributeSize(nat, MF_MT_FRAME_SIZE, &nw, &nh);
    }
    nat = nullptr;

    UINT32 capW = 0, capH = 0;
    GUID capS = GUID_NULL;
    bool capNv = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        capW = capW_;
        capH = capH_;
        capS = capSub_;
        capNv = capIsNv12_;
    }
    auto subId = [](const GUID& s) -> int {
        if (s == MFVideoFormat_NV12) return 0;
        if (s == MFVideoFormat_RGB32 || s == MFVideoFormat_ARGB32) return 1;
        return 2;
    };
    const bool curSupported = (curSub == MFVideoFormat_NV12) ||
                              (curSub == MFVideoFormat_RGB32) ||
                              (curSub == MFVideoFormat_ARGB32);
    if (FAILED(hc) || cw == 0 || ch == 0 || !curSupported) {
        LogCamera(L"native-type tick: current unreadable/unsupported, keep " +
                  std::to_wstring(capW) + L"x" + std::to_wstring(capH));
        return;
    }
    if (!vcam::camsource::ShouldAdaptToCurrent(capW, capH, subId(capS),
                                               cw, ch, subId(curSub))) {
        LogCamera(L"native-type tick: type unchanged (" + std::to_wstring(cw) +
                  L"x" + std::to_wstring(ch) + L")");
        return;
    }
    // Адаптация: сначала аллоцируем новый кэш вне лока (8 МБ), затем коммитим
    // размеры + swap под мьютексом. OOM — dims не трогаем, продолжаем со старым.
    std::vector<uint8_t> fresh;
    try {
        fresh.resize((size_t)cw * ch * 4);
    } catch (...) {
        LogCamera(L"native-type tick: out of memory for " + std::to_wstring(cw) +
                  L"x" + std::to_wstring(ch) + L", keep previous");
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!open_) return;
        capW_ = cw;
        capH_ = ch;
        capStride_ = cstride;
        capSub_ = curSub;
        capIsNv12_ = (curSub == MFVideoFormat_NV12);
        cache_.swap(fresh);
        hasFrame_ = false;
    }
    LogCamera(L"native-type tick: adapted to " + std::to_wstring(cw) + L"x" +
              std::to_wstring(ch) + L", out=" +
              ((curSub == MFVideoFormat_NV12) ? L"NV12" : L"RGB32"));
    (void)natSub; (void)nw; (void)nh; (void)capNv;
}

// Фоновый захват: ReadSample -> свежий RGB32-кэш (только при новом сэмпле;
// NV12-натив конвертируется CPU BT.601 в точке копирования — кэш/даунстрим
// остаются RGB32). Нулевой сэмпл с NATIVEMEDIATYPECHANGED (0x100) — тик смены
// натива: silent сбрасывается, перечитываем типы (адаптация без failed).
// Счётчик молчания: N подряд ошибок/пустых чтений (в т.ч. MF_E_SHUTDOWN) ->
// failed_ -> выход (Render дальше отдаёт false с причиной).
void CameraSource::CaptureLoop()
{
    HRESULT hrCo = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool coHere = (hrCo == S_OK);

    IMFSourceReader* rdr = reader_;
    if (!rdr) {
        SetFailed(L"reader отсутствует");
        if (coHere) CoUninitialize();
        return;
    }

    int silent = 0;
    for (;;) {
        if (WaitForSingleObject(stopEvent_, 0) == WAIT_OBJECT_0) break;

        DWORD actual = 0;
        DWORD flags = 0;
        LONGLONG ts = 0;
        ATL::CComPtr<IMFSample> sample;
        HRESULT hr = rdr->ReadSample(streamIndex_, 0, &actual, &flags, &ts, &sample);

        if (FAILED(hr)) {
            sample = nullptr;
            if (hr == MF_E_SHUTDOWN) {
                SetFailed(L"устройство закрыто: " + HrHex(hr));
                break;
            }
            silent++;
            if (silent >= kMaxSilent) {
                SetFailed(L"ReadSample failed: " + HrHex(hr));
                break;
            }
            Sleep(10);
            continue;
        }

        if (!sample) {
            // Тик смены натива — НЕ молчание и НЕ смерть: сбросить счётчик,
            // перечитать типы (адаптация/лог внутри) и читать дальше.
            if (vcam::camsource::IsNativeTypeChangedTick(false, flags)) {
                silent = 0;
                OnNativeTypeChangedTick(rdr);
                continue;
            }
            silent++;
            if (silent >= kMaxSilent) {
                SetFailed(L"камера не отдаёт кадры ~" +
                          std::to_wstring(kMaxSilent) + L" итераций");
                break;
            }
            continue;
        }
        // Сэмпл с флагом смены типа (редкий кейс): сначала requery, затем
        // обрабатываем этот же сэмпл уже новыми dims.
        if ((flags & 0x100u) != 0u) {
            silent = 0;
            OnNativeTypeChangedTick(rdr);
        } else {
            silent = 0;
        }

        // Снапшот dims под мьютексом (тик в этом же потоке мог их поменять).
        UINT32 w = 0, h = 0;
        LONG sstride = 0;
        bool isNv12 = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!open_) continue;
            w = capW_;
            h = capH_;
            sstride = capStride_;
            isNv12 = capIsNv12_;
            if (w == 0 || h == 0 || cache_.size() < (size_t)w * h * 4) continue;
        }

        ATL::CComPtr<IMFMediaBuffer> buf;
        if (SUCCEEDED(sample->ConvertToContiguousBuffer(&buf)) && buf) {
            BYTE* data = nullptr;
            DWORD maxLen = 0;
            DWORD curLen = 0;
            if (SUCCEEDED(buf->Lock(&data, &maxLen, &curLen)) && data) {
                if (isNv12) {
                    const size_t sAbs =
                        (size_t)(sstride >= 0 ? sstride : -sstride);
                    const size_t need =
                        sAbs * (size_t)h + sAbs * (size_t)((h + 1u) / 2u);
                    if (curLen >= need && sAbs >= (size_t)w) {
                        std::lock_guard<std::mutex> lock(mutex_);
                        if (!cache_.empty() && open_ && capW_ == w &&
                            capH_ == h && cache_.size() >= (size_t)w * h * 4) {
                            vcam::camsource::Nv12ToRgb32(
                                data, w, h, sstride, cache_.data(),
                                (LONG)(w * 4));
                            hasFrame_ = true;
                        }
                    }
                } else {
                    const size_t rowBytes = (size_t)w * 4;
                    if (curLen >= rowBytes * h) {
                        std::lock_guard<std::mutex> lock(mutex_);
                        if (!cache_.empty() && open_ && capW_ == w &&
                            capH_ == h) {
                            for (UINT32 y = 0; y < h; y++)
                                memcpy(cache_.data() + (size_t)y * rowBytes,
                                       RowPtr(data, sstride, h, y), rowBytes);
                            hasFrame_ = true;
                        }
                    }
                }
                buf->Unlock();
            }
        }
    }

    if (coHere) CoUninitialize();
}
