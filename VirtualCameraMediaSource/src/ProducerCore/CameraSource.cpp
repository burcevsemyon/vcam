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

// Близость нативного типа к 1280x720: аспект 16:9 -> 0, затем площадь.
int64_t NativeScore(const CamNativeType& c)
{
    int64_t aspectDiff = llabs((int64_t)c.w * 720 - (int64_t)c.h * 1280);
    int64_t areaDiff = llabs((int64_t)c.w * c.h - 1280LL * 720);
    return aspectDiff * 4 + areaDiff / 100;
}

// Переговоры о выходе RGB32: сначала 1280x720@30, затем перебор нативных
// типов (предпочесть RGB32 ближайший к 1280x720), затем RGB32 поверх нативных
// размеров (NV12/YUY2/MJPG конвертирует video processing).
bool ConfigureCameraReader(IMFSourceReader* rdr, DWORD& outStream, UINT32& outW,
                           UINT32& outH, LONG& outStride, std::wstring& err)
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

    HRESULT hr = trySet(MFVideoFormat_RGB32, vcam::VCamWidth, vcam::VCamHeight, true);
    if (FAILED(hr)) hr = trySet(MFVideoFormat_RGB32, vcam::VCamWidth, vcam::VCamHeight, false);

    if (FAILED(hr) && !natives.empty()) {
        std::stable_sort(natives.begin(), natives.end(),
                         [](const CamNativeType& a, const CamNativeType& b) {
                             return NativeScore(a) < NativeScore(b);
                         });
        // 1) нативные уже в RGB32 — своим размером
        for (const CamNativeType& n : natives) {
            if (n.sub != MFVideoFormat_RGB32) continue;
            hr = trySet(n.sub, n.w, n.h, true);
            if (FAILED(hr)) hr = trySet(n.sub, n.w, n.h, false);
            if (SUCCEEDED(hr)) break;
        }
        // 2) RGB32 поверх нативных размеров (video processing конвертирует)
        if (FAILED(hr)) {
            for (const CamNativeType& n : natives) {
                hr = trySet(MFVideoFormat_RGB32, n.w, n.h, true);
                if (FAILED(hr)) hr = trySet(MFVideoFormat_RGB32, n.w, n.h, false);
                if (SUCCEEDED(hr)) break;
            }
        }
    }
    if (FAILED(hr)) { err = L"камера не даёт RGB32: " + HrHex(hr); return false; }

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
    if (sub != MFVideoFormat_RGB32 && sub != MFVideoFormat_ARGB32) {
        err = L"выход не RGB32 (конвертация недоступна)";
        return false;
    }
    if (stride == 0) stride = (LONG)(w * 4);

    outStream = vid;
    outW = w;
    outH = h;
    outStride = stride;
    return true;
}

// Перебор режимов ридера: video processing + advanced (Win10+, если атрибут
// поддерживается) -> video processing -> plain. Успех = reader создался И
// выход RGB32 согласован.
// Путь открытия: IMFActivate::ActivateObject -> MFCreateSourceReaderFromMediaSource.
// MFCreateSourceReaderFromURL(symlink) на этом стенде даёт 0x80070002
// (ERROR_FILE_NOT_FOUND) — диагностика, спека неточна.
bool OpenCameraReader(IMFActivate* act, ATL::CComPtr<IMFSourceReader>& out,
                      ATL::CComPtr<IMFMediaSource>& outSrc,
                      DWORD& outStream, UINT32& outW, UINT32& outH, LONG& outStride,
                      std::wstring& err)
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
        std::wstring cerr;
        if (ConfigureCameraReader(rdr, st, w, h, stride, cerr)) {
            out = std::move(rdr);
            outSrc = std::move(msrc);
            outStream = st;
            outW = w;
            outH = h;
            outStride = stride;
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
    bool readerOk = OpenCameraReader(act, rdr, msrc, stream, w, h, stride, err);
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
              std::to_wstring(capH_) + L")");
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

// Фоновый захват: ReadSample -> свежий RGB32-кэш (только при новом сэмпле).
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
            silent++;
            if (silent >= kMaxSilent) {
                SetFailed(L"камера не отдаёт кадры ~" +
                          std::to_wstring(kMaxSilent) + L" итераций");
                break;
            }
            continue;
        }
        silent = 0;

        ATL::CComPtr<IMFMediaBuffer> buf;
        if (SUCCEEDED(sample->ConvertToContiguousBuffer(&buf)) && buf) {
            BYTE* data = nullptr;
            DWORD maxLen = 0;
            DWORD curLen = 0;
            if (SUCCEEDED(buf->Lock(&data, &maxLen, &curLen)) && data) {
                const size_t rowBytes = (size_t)capW_ * 4;
                if (curLen >= rowBytes * capH_) {
                    std::lock_guard<std::mutex> lock(mutex_);
                    if (!cache_.empty() && open_) {
                        for (UINT32 y = 0; y < capH_; y++)
                            memcpy(cache_.data() + (size_t)y * rowBytes,
                                   RowPtr(data, capStride_, capH_, y), rowBytes);
                        hasFrame_ = true;
                    }
                }
                buf->Unlock();
            }
        }
    }

    if (coHere) CoUninitialize();
}
