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

std::wstring HrHex(HRESULT hr)
{
    wchar_t buf[16];
    swprintf(buf, 16, L"0x%08X", (unsigned)hr);
    return std::wstring(buf);
}

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

// Selects the first video stream and negotiates an RGB32 output (перенос из
// VideoProducer.cpp: без MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING ридер отклоняет
// голый RGB32 — MF_E_INVALIDMEDIATYPE).
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

    bool aspectPreserved = false;
    if (srcW > 0 && srcH > 0) {
        double want = (double)vcam::VCamWidth / vcam::VCamHeight;
        double have = (double)srcW / srcH;
        aspectPreserved = fabs(have - want) <= want * 0.01;
    }

    struct Candidate {
        GUID subtype;
        UINT32 w;
        UINT32 h;
        bool interlace;
        const wchar_t* name;
    };
    const Candidate cands[] = {
        { MFVideoFormat_RGB32,  vcam::VCamWidth, vcam::VCamHeight, false, L"RGB32 1280x720" },
        { MFVideoFormat_RGB32,  srcW, srcH, true,  L"RGB32 native progressive" },
        { MFVideoFormat_RGB32,  srcW, srcH, false, L"RGB32 native" },
        { MFVideoFormat_RGB32,  0,    0,    false, L"RGB32 no size" },
        { MFVideoFormat_ARGB32, srcW, srcH, false, L"ARGB32 native" },
    };

    HRESULT hrSet = E_FAIL;
    const wchar_t* usedCand = nullptr;
    for (const Candidate& c : cands) {
        if (c.w && !aspectPreserved &&
            c.w == vcam::VCamWidth && c.h == vcam::VCamHeight) continue;
        if (c.w && c.w != vcam::VCamWidth && (c.w != srcW || c.h != srcH)) continue;
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

void VideoFileSource::RenderToFrame(const BYTE* data, UINT w, UINT h, LONG stride, BYTE* dst)
{
    if (w == vcam::VCamWidth && h == vcam::VCamHeight &&
        stride == (LONG)vcam::VCamStride) {
        for (UINT y = 0; y < h; y++) {
            memcpy(dst + (size_t)y * vcam::VCamStride, RowPtr(data, stride, h, y), vcam::VCamStride);
        }
        return;
    }
    // Reader отдаёт RGB32 (кандидаты ConfigureReader — RGB32/ARGB32 native,
    // layout идентичен): сначала Video Processor MFT, при false — CPU.
    if (mftScaler_.Scale(data, w, h, stride, dst))
        return;
    LetterboxBilinear(data, w, h, stride, dst, (LONG)vcam::VCamStride);
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
    reuse = open_ && !failed_ && cfg == cfg_;
    LeaveCriticalSection(&cs_);
    if (reuse) return true;

    // Текущий decode-поток обязан остановиться, иначе Open перезаписал бы
    // stopEvent_/frame_ под живым DecodeLoop.
    if (!Shutdown(3000)) {
        err = L"previous decode thread is still running";
        return false;
    }

    if (cfg.path.empty()) { err = L"empty media path"; return false; }

    try {
        frame_.resize(vcam::VCamFrameSize);
    } catch (...) {
        err = L"out of memory";
        return false;
    }
    memset(frame_.data(), 0, frame_.size());

    cfg_ = cfg;
    EnterCriticalSection(&cs_);
    frameReady_ = false;
    failed_ = false;
    failReason_.clear();
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
    else if (!frameReady_) err = L"no frame decoded yet";
    else ready = true;

    if (ready) {
        vcam::CopyFrameRowwise(bgrx, (size_t)stride, frame_.data(), vcam::VCamStride,
                               vcam::VCamWidth, vcam::VCamHeight, vcam::VCamPixelSize);
    }
    LeaveCriticalSection(&cs_);
    return ready;
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
    LeaveCriticalSection(&cs_);

    frame_.clear();
    frame_.shrink_to_fit();
    cfg_ = SourceConfig();
    return true;
}

DWORD WINAPI VideoFileSource::ThreadProc(LPVOID self)
{
    static_cast<VideoFileSource*>(self)->DecodeLoop();
    return 0;
}

// Фоновый декод: pacing по timestamp (frame-holding), letterbox в frame_ под cs_,
// луп SetPosition(0) на end-of-stream. Ошибка → SetFailed (Render даст false).
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
                    if (!frame_.empty()) {
                        RenderToFrame(data, vs.outW, vs.outH, vs.stride, frame_.data());
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
