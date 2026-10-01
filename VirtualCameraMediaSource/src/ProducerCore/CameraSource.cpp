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
#include "SharedMemoryContract.h"

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mf.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")

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

const BYTE* RowPtr(const BYTE* data, LONG stride, UINT h, UINT y)
{
    if (stride >= 0) return data + (size_t)y * (size_t)stride;
    return data + (size_t)(h - 1 - y) * (size_t)(-stride);
}

void LetterboxNearest(const BYTE* src, UINT sw, UINT sh, LONG sstride, BYTE* dst, LONG dstride)
{
    const UINT tw = vcam::VCamWidth, th = vcam::VCamHeight;
    double scale = (double)tw / sw;
    if ((double)th / sh < scale) scale = (double)th / sh;
    int dw = (int)llround((double)sw * scale);
    int dh = (int)llround((double)sh * scale);
    if (dw < 1) dw = 1;
    if (dh < 1) dh = 1;
    if (dw > (int)tw) dw = (int)tw;
    if (dh > (int)th) dh = (int)th;
    int x0 = ((int)tw - dw) / 2;
    int y0 = ((int)th - dh) / 2;

    for (int y = 0; y < dh; y++) {
        UINT sy = (UINT)((int64_t)y * sh / dh);
        if (sy >= sh) sy = sh - 1;
        const BYTE* row = RowPtr(src, sstride, sh, sy);
        BYTE* drow = dst + (size_t)(y0 + y) * dstride + (size_t)x0 * 4;
        for (int x = 0; x < dw; x++) {
            UINT sx = (UINT)((int64_t)x * sw / dw);
            if (sx >= sw) sx = sw - 1;
            const BYTE* p = row + (size_t)sx * 4;
            BYTE* q = drow + (size_t)x * 4;
            q[0] = p[0]; q[1] = p[1]; q[2] = p[2]; q[3] = p[3];
        }
    }
}

// Честный fit (letterbox/pillarbox): сохраняет пропорции, центрирует,
// остальное — чёрное. Fixed-point 16.16 билинейный (паттерн из VideoFileSource).
void LetterboxBilinear(const BYTE* src, UINT sw, UINT sh, LONG sstride, BYTE* dst, LONG dstride)
{
    const UINT tw = vcam::VCamWidth, th = vcam::VCamHeight;
    memset(dst, 0, (size_t)dstride * th);
    if (sw == 0 || sh == 0) return;
    if (sw < 2 || sh < 2) {
        LetterboxNearest(src, sw, sh, sstride, dst, dstride);
        return;
    }

    double scale = (double)tw / sw;
    if ((double)th / sh < scale) scale = (double)th / sh;
    int dw = (int)llround((double)sw * scale);
    int dh = (int)llround((double)sh * scale);
    if (dw < 1) dw = 1;
    if (dh < 1) dh = 1;
    if (dw > (int)tw) dw = (int)tw;
    if (dh > (int)th) dh = (int)th;
    int x0 = ((int)tw - dw) / 2;
    int y0 = ((int)th - dh) / 2;

    const int64_t stepX = ((int64_t)sw << 16) / dw;
    const int64_t stepY = ((int64_t)sh << 16) / dh;
    const int64_t maxX = ((int64_t)(sw - 1) << 16);
    const int64_t maxY = ((int64_t)(sh - 1) << 16);

    for (int y = 0; y < dh; y++) {
        int64_t sy16 = (int64_t)y * stepY + stepY / 2 - 32768;
        if (sy16 < 0) sy16 = 0;
        if (sy16 > maxY) sy16 = maxY;
        UINT sy0 = (UINT)(sy16 >> 16);
        UINT fy = (UINT)(sy16 & 0xFFFF);
        if (sy0 >= sh - 1) { sy0 = sh - 2; fy = 0xFFFF; }
        UINT sy1 = sy0 + 1;

        const BYTE* r0 = RowPtr(src, sstride, sh, sy0);
        const BYTE* r1 = RowPtr(src, sstride, sh, sy1);
        BYTE* drow = dst + (size_t)(y0 + y) * dstride + (size_t)x0 * 4;
        const int64_t wy0 = 65536 - fy;
        const int64_t wy1 = fy;

        for (int x = 0; x < dw; x++) {
            int64_t sx16 = (int64_t)x * stepX + stepX / 2 - 32768;
            if (sx16 < 0) sx16 = 0;
            if (sx16 > maxX) sx16 = maxX;
            UINT sx0 = (UINT)(sx16 >> 16);
            UINT fx = (UINT)(sx16 & 0xFFFF);
            if (sx0 >= sw - 1) { sx0 = sw - 2; fx = 0xFFFF; }
            UINT sx1 = sx0 + 1;
            const int64_t wx0 = 65536 - fx;
            const int64_t wx1 = fx;

            const BYTE* p00 = r0 + (size_t)sx0 * 4;
            const BYTE* p01 = r0 + (size_t)sx1 * 4;
            const BYTE* p10 = r1 + (size_t)sx0 * 4;
            const BYTE* p11 = r1 + (size_t)sx1 * 4;
            BYTE* q = drow + (size_t)x * 4;

            for (int c = 0; c < 4; c++) {
                int64_t top = (int64_t)p00[c] * wx0 + (int64_t)p01[c] * wx1;
                int64_t bot = (int64_t)p10[c] * wx0 + (int64_t)p11[c] * wx1;
                q[c] = (BYTE)((top * wy0 + bot * wy1) >> 32);
            }
        }
    }
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
        IMFMediaType* mt = nullptr;
        if (FAILED(rdr->GetNativeMediaType(i, 0, &mt)) || !mt) break;
        GUID maj = GUID_NULL;
        mt->GetMajorType(&maj);
        mt->Release();
        if (maj == MFMediaType_Video) { vid = i; break; }
    }
    if (vid == MAXDWORD) { err = L"нет видеопотока"; return false; }

    rdr->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE);
    rdr->SetStreamSelection(vid, TRUE);

    std::vector<CamNativeType> natives;
    for (DWORD i = 0;; i++) {
        IMFMediaType* mt = nullptr;
        if (FAILED(rdr->GetNativeMediaType(vid, i, &mt)) || !mt) break;
        CamNativeType n;
        GUID maj = GUID_NULL;
        mt->GetGUID(MF_MT_MAJOR_TYPE, &maj);
        mt->GetGUID(MF_MT_SUBTYPE, &n.sub);
        MFGetAttributeSize(mt, MF_MT_FRAME_SIZE, &n.w, &n.h);
        mt->Release();
        if (maj == MFMediaType_Video && n.w && n.h) natives.push_back(n);
    }

    auto trySet = [&](const GUID& sub, UINT32 w, UINT32 h, bool fps) -> HRESULT {
        IMFMediaType* p = nullptr;
        if (FAILED(MFCreateMediaType(&p)) || !p) return E_OUTOFMEMORY;
        p->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        p->SetGUID(MF_MT_SUBTYPE, sub);
        p->SetUINT64(MF_MT_FRAME_SIZE, ((UINT64)w << 32) | (UINT64)h);
        if (fps)
            p->SetUINT64(MF_MT_FRAME_RATE, ((UINT64)kFps << 32) | 1ULL);
        p->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        HRESULT hr = rdr->SetCurrentMediaType(vid, nullptr, p);
        p->Release();
        return hr;
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

    IMFMediaType* cur = nullptr;
    HRESULT hc = rdr->GetCurrentMediaType(vid, &cur);
    if (FAILED(hc) || !cur) { err = L"GetCurrentMediaType failed: " + HrHex(hc); return false; }
    GUID sub = GUID_NULL;
    cur->GetGUID(MF_MT_SUBTYPE, &sub);
    UINT64 fs = 0;
    if (FAILED(cur->GetUINT64(MF_MT_FRAME_SIZE, &fs))) fs = 0;
    LONG stride = 0;
    if (FAILED(cur->GetUINT32(MF_MT_DEFAULT_STRIDE, (UINT32*)&stride))) stride = 0;
    cur->Release();

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
bool OpenCameraReader(IMFActivate* act, IMFSourceReader*& out, IMFMediaSource*& outSrc,
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
        IMFMediaSource* msrc = nullptr;
        HRESULT hr = act->ActivateObject(IID_IMFMediaSource, (void**)&msrc);
        if (FAILED(hr) || !msrc) {
            lastHr = hr;
            lastErr = L"ActivateObject failed: " + HrHex(hr);
            continue;
        }

        IMFAttributes* attr = nullptr;
        if (p.vp || p.adv) {
            if (SUCCEEDED(MFCreateAttributes(&attr, 2)) && attr) {
                if (p.vp) attr->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
#ifdef MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING
                if (p.adv)
                    attr->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);
#endif
            }
        }
        IMFSourceReader* rdr = nullptr;
        hr = MFCreateSourceReaderFromMediaSource(msrc, attr, &rdr);
        if (attr) attr->Release();
        if (FAILED(hr) || !rdr) {
            lastHr = hr;
            lastErr = L"MFCreateSourceReaderFromMediaSource failed: " + HrHex(hr);
            msrc->Shutdown();
            msrc->Release();
            continue;
        }

        DWORD st = 0;
        UINT32 w = 0, h = 0;
        LONG stride = 0;
        std::wstring cerr;
        if (ConfigureCameraReader(rdr, st, w, h, stride, cerr)) {
            out = rdr;
            outSrc = msrc;
            outStream = st;
            outW = w;
            outH = h;
            outStride = stride;
            LogCamera(std::wstring(L"reader mode: ") + p.name);
            return true;
        }
        lastErr = cerr;
        rdr->Release();
        msrc->Shutdown();
        msrc->Release();
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
    IMFActivate* act = OpenCameraActivate(cfg.path, cfg.camName, devInfo);
    if (!act) {
        err = L"камера не найдена: " + DescribeTarget(cfg);
        Close(); // парный MFShutdown/CoUninitialize
        return false;
    }
    if (devInfo.id.empty()) {
        act->Release();
        err = L"камера без symlink: " + DescribeTarget(cfg);
        Close();
        return false;
    }

    IMFSourceReader* rdr = nullptr;
    IMFMediaSource* msrc = nullptr;
    DWORD stream = 0;
    UINT32 w = 0, h = 0;
    LONG stride = 0;
    bool readerOk = OpenCameraReader(act, rdr, msrc, stream, w, h, stride, err);
    act->Release();
    if (!readerOk) {
        Close(); // парный MFShutdown/CoUninitialize
        return false;
    }
    mediaSrc_ = msrc;
    reader_ = rdr;
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

    stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
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

    thread_ = CreateThread(nullptr, 0, ThreadProc, this, 0, nullptr);
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
        if (stride == (int)vcam::VCamStride) {
            memcpy(bgrx, cache_.data(), vcam::VCamFrameSize);
        } else {
            for (UINT32 y = 0; y < vcam::VCamHeight; y++)
                memcpy(bgrx + (size_t)y * (size_t)stride,
                       cache_.data() + (size_t)y * vcam::VCamStride, vcam::VCamStride);
        }
    } else {
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
        CloseHandle(thread_);
        thread_ = nullptr;
    }

    if (stopEvent_) { CloseHandle(stopEvent_); stopEvent_ = nullptr; }
    if (reader_) { reader_->Release(); reader_ = nullptr; }
    if (mediaSrc_) {
        mediaSrc_->Shutdown();
        mediaSrc_->Release();
        mediaSrc_ = nullptr;
    }

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
        IMFSample* sample = nullptr;
        HRESULT hr = rdr->ReadSample(streamIndex_, 0, &actual, &flags, &ts, &sample);

        if (FAILED(hr)) {
            if (sample) { sample->Release(); sample = nullptr; }
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

        IMFMediaBuffer* buf = nullptr;
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
            buf->Release();
        }
        sample->Release();
    }

    if (coHere) CoUninitialize();
}
