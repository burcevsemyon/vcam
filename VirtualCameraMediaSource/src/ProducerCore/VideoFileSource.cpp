#include "VideoFileSource.h"

#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <propidl.h>
#include <atlbase.h>

#include <cmath>
#include <cstring>

#include "FrameCopy.h"
#include "ImageLayout.h"
#include "SharedMemoryContract.h"
#include "WinUtil.h"

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mf.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")

using vcam::RowPtr;
using vcam::LetterboxBilinear;

namespace {

void LogVideo(const std::wstring& msg)
{
    OutputDebugStringW((L"[ProducerCore:video] " + msg + L"\n").c_str());
}

using vcam::HrHex;

struct VideoState {
    ATL::CComPtr<IMFSourceReader> reader;
    DWORD streamIndex = 0;
    UINT outW = 0;
    UINT outH = 0;
    LONG stride = 0;
};

void CloseVideo(VideoState& v)
{
    v.reader = nullptr;
    v.outW = v.outH = 0;
    v.stride = 0;
}

// Нативный размер декода (кламп fit'ом к cap 4K): больше cap — просим у ридера
// cap-fit (обычно отклоняет — тогда декодим натив и даунскейлим CPU в кадре).
void FitCap(UINT w, UINT h, UINT& outW, UINT& outH)
{
    if (w <= vcam::VCamNativeCapW && h <= vcam::VCamNativeCapH) {
        outW = w;
        outH = h;
        return;
    }
    double s = (double)vcam::VCamNativeCapW / w;
    double s2 = (double)vcam::VCamNativeCapH / h;
    if (s2 < s) s = s2;
    outW = (UINT)llround(w * s);
    outH = (UINT)llround(h * s);
    if (outW < 1) outW = 1;
    if (outH < 1) outH = 1;
    if (outW > vcam::VCamNativeCapW) outW = vcam::VCamNativeCapW;
    if (outH > vcam::VCamNativeCapH) outH = vcam::VCamNativeCapH;
}

// Selects the first video stream and negotiates an RGB32 output (перенос из
// VideoProducer.cpp: без MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING ридер отклоняет
// голый RGB32 — MF_E_INVALIDMEDIATYPE).
// v2: предпочтение — НАТИВНЫЙ размер декода (cap-fit при переборе), 720p —
// лишь fallback для привередливых ридеров (раньше 720p просили первым).
bool ConfigureReader(IMFSourceReader* rdr, VideoState& out, std::wstring& err)
{
    DWORD vid = MAXDWORD;
    for (DWORD i = 0; i < 128; i++) {
        ATL::CComPtr<IMFMediaType> mt;
        if (FAILED(rdr->GetNativeMediaType(i, 0, &mt)) || !mt) break;
        GUID maj = GUID_NULL;
        mt->GetMajorType(&maj);
        if (maj == MFMediaType_Video) { vid = i; break; }
    }
    if (vid == MAXDWORD) { err = L"no video stream"; return false; }

    rdr->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE);
    rdr->SetStreamSelection(vid, TRUE);

    UINT32 srcW = 0, srcH = 0;
    ATL::CComPtr<IMFMediaType> pNat;
    if (SUCCEEDED(rdr->GetNativeMediaType(vid, 0, &pNat)) && pNat) {
        UINT64 fs = 0;
        if (SUCCEEDED(pNat->GetUINT64(MF_MT_FRAME_SIZE, &fs))) {
            srcW = (UINT32)(fs >> 32);
            srcH = (UINT32)(fs & 0xFFFFFFFFu);
        }
    }

    UINT capW = srcW, capH = srcH;
    if (srcW && srcH) FitCap(srcW, srcH, capW, capH);

    struct Candidate {
        GUID subtype;
        UINT32 w;
        UINT32 h;
        bool interlace;
        const wchar_t* name;
    };
    const Candidate cands[] = {
        { MFVideoFormat_RGB32,  capW, capH, true,  L"RGB32 cap-fit progressive" },
        { MFVideoFormat_RGB32,  capW, capH, false, L"RGB32 cap-fit" },
        { MFVideoFormat_RGB32,  srcW, srcH, true,  L"RGB32 native progressive" },
        { MFVideoFormat_RGB32,  srcW, srcH, false, L"RGB32 native" },
        { MFVideoFormat_RGB32,  0,    0,    false, L"RGB32 no size" },
        { MFVideoFormat_ARGB32, srcW, srcH, false, L"ARGB32 native" },
        { MFVideoFormat_RGB32,  vcam::VCamWidth, vcam::VCamHeight, false, L"RGB32 1280x720" },
    };

    HRESULT hrSet = E_FAIL;
    const wchar_t* usedCand = nullptr;
    // Порядок = предпочтение: cap-fit натив -> натив -> без размера -> ARGB ->
    // 720p-совместимость. Дубли (cap-fit == натив в пределах cap) безвредны:
    // цикл рвётся на первом успехе.
    for (const Candidate& c : cands) {
        ATL::CComPtr<IMFMediaType> pOut;
        if (FAILED(MFCreateMediaType(&pOut)) || !pOut) continue;
        pOut->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        pOut->SetGUID(MF_MT_SUBTYPE, c.subtype);
        if (c.w && c.h) pOut->SetUINT64(MF_MT_FRAME_SIZE, ((UINT64)c.w << 32) | (UINT64)c.h);
        if (c.interlace) pOut->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        HRESULT hr = rdr->SetCurrentMediaType(vid, nullptr, pOut);
        pOut = nullptr;
        hrSet = hr;
        if (SUCCEEDED(hr)) { usedCand = c.name; break; }
        LogVideo(std::wstring(c.name) + L" -> " + HrHex(hr));
    }

    if (FAILED(hrSet) || !usedCand) {
        err = L"SetCurrentMediaType(RGB) failed: " + HrHex(hrSet);
        return false;
    }
    LogVideo(std::wstring(L"output format: ") + usedCand);

    ATL::CComPtr<IMFMediaType> pCur;
    HRESULT hr = rdr->GetCurrentMediaType(vid, &pCur);
    if (FAILED(hr) || !pCur) { err = L"GetCurrentMediaType failed: " + HrHex(hr); return false; }
    UINT64 cur = 0;
    if (FAILED(pCur->GetUINT64(MF_MT_FRAME_SIZE, &cur))) cur = 0;
    LONG stride = 0;
    if (FAILED(pCur->GetUINT32(MF_MT_DEFAULT_STRIDE, (UINT32*)&stride))) stride = 0;
    pCur = nullptr;

    UINT w = (UINT32)(cur >> 32);
    UINT h = (UINT32)(cur & 0xFFFFFFFFu);
    if (w == 0 || h == 0) { err = L"output frame size is zero"; return false; }
    if (stride == 0) stride = (LONG)(w * 4);

    out.streamIndex = vid;
    out.outW = w;
    out.outH = h;
    out.stride = stride;
    return true;
}

bool OpenVideo(const std::wstring& path, VideoState& out, std::wstring& err)
{
    if (path.empty()) { err = L"empty media path"; return false; }

    static const struct { const GUID* attr; const wchar_t* name; } passes[] = {
        { &MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, L"videoProcessing" },
        { nullptr, L"plain" },
        { &MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, L"advancedVideoProcessing" },
    };

    for (const auto& pass : passes) {
        ATL::CComPtr<IMFAttributes> pAttr;
        if (pass.attr) {
            if (FAILED(MFCreateAttributes(&pAttr, 1)) || !pAttr) pAttr = nullptr;
            else pAttr->SetUINT32(*pass.attr, TRUE);
        }
        ATL::CComPtr<IMFSourceReader> rdr;
        HRESULT hr = MFCreateSourceReaderFromURL(path.c_str(), pAttr, &rdr);
        pAttr = nullptr;
        if (FAILED(hr)) {
            rdr = nullptr;
            err = L"MFCreateSourceReaderFromURL failed: " + HrHex(hr);
            return false;
        }

        VideoState tmp;
        std::wstring e2;
        if (ConfigureReader(rdr, tmp, e2)) {
            tmp.reader = std::move(rdr);
            out = std::move(tmp);
            LogVideo(std::wstring(L"reader mode: ") + pass.name);
            return true;
        }
        err = e2;
        rdr = nullptr;
        LogVideo(std::wstring(L"reader mode ") + pass.name + L" failed: " + e2);
    }
    return false;
}

bool EnsureMfStarted(std::wstring& err)
{
    static LONG state = 0; // 0 = not started, 1 = in progress, 2 = ok, 3 = failed
    for (;;) {
        LONG prev = InterlockedCompareExchange(&state, 1, 0);
        if (prev == 0) {
            HRESULT hr = MFStartup(MF_VERSION, MFSTARTUP_FULL);
            if (FAILED(hr)) {
                err = L"MFStartup failed: " + HrHex(hr);
                InterlockedExchange(&state, 3);
            } else {
                InterlockedExchange(&state, 2);
            }
            return prev == 0 && state == 2;
        }
        if (prev == 1) { Sleep(10); continue; }
        if (prev == 3) { err = L"Media Foundation unavailable"; return false; }
        return true;
    }
}

} // namespace

void VideoFileSource::RenderToFrame(const BYTE* data, UINT w, UINT h, LONG stride)
{
    if (frame_.empty() || frameW_ == 0 || frameH_ == 0) return;
    BYTE* dst = frame_.data();
    const LONG dstride = (LONG)(frameW_ * 4);
    if (w == frameW_ && h == frameH_ && stride == dstride) {
        for (UINT y = 0; y < h; y++) {
            memcpy(dst + (size_t)y * dstride, RowPtr(data, stride, h, y), (size_t)dstride);
        }
        return;
    }
    // frame_ — aspect-fit декода к cap (см. DecodeLoop): точная растяжка CPU.
    // Reader отдаёт RGB32/ARGB32 (layout идентичен).
    vcam::StretchBilinearEx(data, w, h, stride, dst, frameW_, frameH_, dstride);
}

bool VideoFileSource::ScaleFrameTo720pLocked(BYTE* dst, LONG dstride)
{
    if (frameW_ == vcam::VCamWidth && frameH_ == vcam::VCamHeight &&
        dstride == (LONG)vcam::VCamStride) {
        vcam::CopyFrameRowwise(dst, (size_t)dstride, frame_.data(), vcam::VCamStride,
                               vcam::VCamWidth, vcam::VCamHeight, vcam::VCamPixelSize);
        return true;
    }
    if (mftScaler_.Scale(frame_.data(), frameW_, frameH_, (LONG)(frameW_ * 4), dst))
        return true;
    vcam::LetterboxBilinear(frame_.data(), frameW_, frameH_, (LONG)(frameW_ * 4),
                             dst, dstride);
    return true;
}

VideoFileSource::VideoFileSource()
{
    InitializeCriticalSection(&cs_);
}

VideoFileSource::~VideoFileSource()
{
    // Декод завис намертво: дожидаемся бесконечно — удаление cs_ под живым
    // DecodeLoop это UB, хуже ожидания.
    if (!Shutdown(3000) && thread_)
        Shutdown(INFINITE);
    DeleteCriticalSection(&cs_);
}

void VideoFileSource::SetFailed(const std::wstring& reason)
{
    EnterCriticalSection(&cs_);
    failed_ = true;
    failReason_ = reason;
    frameReady_ = false;
    LeaveCriticalSection(&cs_);
    LogVideo(L"failed: " + reason);
}

bool VideoFileSource::Open(const SourceConfig& cfg, std::wstring& err)
{
    bool reuse = false;
    EnterCriticalSection(&cs_);
    // Закончившийся play-once с тем же конфигом не переиспользуем: он уже
    // отдал "ended" и кадров не даст — нужен новый проход с начала файла.
    reuse = open_ && !failed_ && !ended_ && cfg == cfg_;
    LeaveCriticalSection(&cs_);
    if (reuse) return true;

    // Текущий decode-поток обязан остановиться, иначе Open перезаписал бы
    // stopEvent_/frame_ под живым DecodeLoop.
    if (!Shutdown(3000)) {
        err = L"previous decode thread is still running";
        return false;
    }

    if (cfg.path.empty()) { err = L"empty media path"; return false; }

    // frame_ аллоцируется в DecodeLoop, когда ридер согласует нативный размер:
    // до первого кадра NativeSize=false, Render=false (нет данных).
    cfg_ = cfg;
    EnterCriticalSection(&cs_);
    frame_.clear();
    frameW_ = frameH_ = 0;
    frameReady_ = false;
    failed_ = false;
    failReason_.clear();
    playOnce_ = cfg.playOnce;
    ended_ = false;
    LeaveCriticalSection(&cs_);

    stopEvent_.Attach(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!stopEvent_) {
        err = L"CreateEventW failed: " + std::to_wstring(GetLastError());
        frame_.clear();
        return false;
    }
    thread_.Attach(CreateThread(nullptr, 0, ThreadProc, this, 0, nullptr));
    if (!thread_) {
        err = L"CreateThread failed: " + std::to_wstring(GetLastError());
        stopEvent_.Close();
        frame_.clear();
        return false;
    }
    open_ = true;
    return true;
}

bool VideoFileSource::Render(uint8_t* bgrx, int stride, std::wstring& err)
{
    if (!bgrx || stride < (int)vcam::VCamStride) { err = L"invalid target buffer"; return false; }

    bool ready = false;
    EnterCriticalSection(&cs_);
    if (!open_) err = L"video source is not open";
    else if (failed_) err = failReason_;
    else if (ended_) err = L"ended";
    else if (!frameReady_) err = L"no frame decoded yet";
    else ready = ScaleFrameTo720pLocked(bgrx, (LONG)stride);
    LeaveCriticalSection(&cs_);
    return ready;
}

bool VideoFileSource::Render(uint8_t* dst, int stride, uint32_t w, uint32_t h,
                             std::wstring& err)
{
    if (!dst || w == 0 || h == 0 || w > vcam::VCamNativeCapW || h > vcam::VCamNativeCapH ||
        stride < (int)(w * 4)) {
        err = L"invalid target buffer";
        return false;
    }

    bool ready = false;
    EnterCriticalSection(&cs_);
    if (!open_) err = L"video source is not open";
    else if (failed_) err = failReason_;
    else if (ended_) err = L"ended";
    else if (!frameReady_) err = L"no frame decoded yet";
    else if (w == frameW_ && h == frameH_) {
        vcam::CopyFrameRowwise(dst, (size_t)stride, frame_.data(), (size_t)frameW_ * 4,
                               frameW_, frameH_, vcam::VCamPixelSize);
        ready = true;
    } else if (w == vcam::VCamWidth && h == vcam::VCamHeight) {
        ready = ScaleFrameTo720pLocked(dst, (LONG)stride);
    } else {
        vcam::LetterboxBilinearEx(frame_.data(), frameW_, frameH_, (LONG)(frameW_ * 4),
                                  dst, w, h, (LONG)stride);
        ready = true;
    }
    LeaveCriticalSection(&cs_);
    return ready;
}

bool VideoFileSource::NativeSize(uint32_t& w, uint32_t& h)
{
    EnterCriticalSection(&cs_);
    bool ok = open_ && !failed_ && frameReady_ && frameW_ != 0 && frameH_ != 0;
    w = ok ? frameW_ : vcam::VCamWidth;
    h = ok ? frameH_ : vcam::VCamHeight;
    LeaveCriticalSection(&cs_);
    return ok;
}

bool VideoFileSource::Ended() const
{
    EnterCriticalSection(const_cast<CRITICAL_SECTION*>(&cs_));
    bool e = ended_;
    LeaveCriticalSection(const_cast<CRITICAL_SECTION*>(&cs_));
    return e;
}

void VideoFileSource::Close()
{
    Shutdown(3000);
}

bool VideoFileSource::Shutdown(DWORD timeoutMs)
{
    if (stopEvent_) SetEvent(stopEvent_);
    if (thread_) {
        if (WaitForSingleObject(thread_, timeoutMs) != WAIT_OBJECT_0)
            return false;   // поток жив: ресурсы/состояние не трогаем
        thread_.Close();
    }
    stopEvent_.Close();

    EnterCriticalSection(&cs_);
    open_ = false;
    frameReady_ = false;
    failed_ = false;
    failReason_.clear();
    playOnce_ = false;
    ended_ = false;
    LeaveCriticalSection(&cs_);

    frame_.clear();
    frame_.shrink_to_fit();
    frameW_ = frameH_ = 0;
    cfg_ = SourceConfig();
    return true;
}

DWORD WINAPI VideoFileSource::ThreadProc(LPVOID self)
{
    static_cast<VideoFileSource*>(self)->DecodeLoop();
    return 0;
}

// Фоновый декод: pacing по timestamp (frame-holding), letterbox в frame_ под cs_,
// луп SetPosition(0) на end-of-stream (default) или ended_-стоп без seek в
// режиме playOnce. Ошибка → SetFailed (Render даст false).
void VideoFileSource::DecodeLoop()
{
    HRESULT hrCo = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool coInited = SUCCEEDED(hrCo);

    std::wstring mfErr;
    if (!EnsureMfStarted(mfErr)) {
        SetFailed(mfErr);
        if (coInited) CoUninitialize();
        return;
    }

    VideoState vs;
    std::wstring err;
    if (!OpenVideo(cfg_.path, vs, err)) {
        SetFailed(err);
        if (coInited) CoUninitialize();
        return;
    }
    LogVideo(L"opened: " + cfg_.path + L" (" + std::to_wstring(vs.outW) + L"x" +
             std::to_wstring(vs.outH) + L" stride=" + std::to_wstring(vs.stride) + L")");

    ULONGLONG wallStart = GetTickCount64();
    LONGLONG baseMs = 0;
    bool failed = false;
    // playOnce_ стабилен на время жизни потока (пишется в Open/Shutdown под
    // остановленный DecodeLoop) — копируем в локальную для проверки EOS.
    const bool playOnce = playOnce_;

    while (WaitForSingleObject(stopEvent_, 0) != WAIT_OBJECT_0) {
        ATL::CComPtr<IMFMediaBuffer> buf;
        DWORD actualStream = 0;
        DWORD flags = 0;
        LONGLONG ts = 0;
        ATL::CComPtr<IMFSample> sample;
        HRESULT hr = vs.reader->ReadSample(vs.streamIndex, 0, &actualStream, &flags, &ts, &sample);

        if (FAILED(hr)) {
            SetFailed(L"ReadSample failed: " + HrHex(hr));
            failed = true;
            break;
        }

        if (sample) {
            ULONGLONG now = GetTickCount64() - wallStart;
            LONGLONG target = baseMs + ts / 10000;
            if ((LONGLONG)now > target + 250) {
                baseMs = (LONGLONG)now - ts / 10000;
                target = now;
            }
            if (target > (LONGLONG)now) {
                DWORD wait = (DWORD)(target - now);
                if (WaitForSingleObject(stopEvent_, wait) != WAIT_TIMEOUT) {
                    sample = nullptr;
                    break;
                }
            }

            if (SUCCEEDED(sample->ConvertToContiguousBuffer(&buf)) && buf) {
                BYTE* data = nullptr;
                DWORD maxLen = 0, curLen = 0;
                if (SUCCEEDED(buf->Lock(&data, &maxLen, &curLen)) && data) {
                    EnterCriticalSection(&cs_);
                    // Первый сэмпл фиксирует нативные размеры (кламп к cap);
                    // frame_ дальше только перезаписывается (ридер размер не меняет).
                    if (frameW_ == 0 || frameH_ == 0) {
                        UINT fw = vs.outW, fh = vs.outH;
                        FitCap(vs.outW, vs.outH, fw, fh);
                        try {
                            frame_.resize((size_t)fw * fh * 4);
                        } catch (...) {
                            frame_.clear();
                            fw = fh = 0;
                        }
                        frameW_ = fw;
                        frameH_ = fh;
                        if (fw && fh) {
                            LogVideo(L"native frame: " + std::to_wstring(vs.outW) + L"x" +
                                     std::to_wstring(vs.outH) + L" -> " +
                                     std::to_wstring(fw) + L"x" + std::to_wstring(fh));
                        }
                    }
                    if (!frame_.empty()) {
                        RenderToFrame(data, vs.outW, vs.outH, vs.stride);
                        frameReady_ = true;
                    }
                    LeaveCriticalSection(&cs_);
                    buf->Unlock();
                }
                buf = nullptr;
            }
            sample = nullptr;
        }

        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
            if (playOnce) {
                // Один проход сыгран: кадров больше не будет, seek не делаем.
                // Последний кадр остаётся в frame_, но Render даёт "ended".
                EnterCriticalSection(&cs_);
                ended_ = true;
                LeaveCriticalSection(&cs_);
                LogVideo(L"end of stream (play-once, no loop)");
                break;
            }
            PROPVARIANT pv;
            PropVariantInit(&pv);
            pv.vt = VT_I8;
            pv.hVal.QuadPart = 0;
            HRESULT hrSeek = vs.reader->SetCurrentPosition(GUID_NULL, pv);
            PropVariantClear(&pv);
            if (FAILED(hrSeek)) {
                SetFailed(L"SetPosition(0) failed: " + HrHex(hrSeek));
                failed = true;
                break;
            }
        }
    }

    (void)failed;
    CloseVideo(vs);
    if (coInited) CoUninitialize();
}
