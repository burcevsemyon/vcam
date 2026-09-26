#include "VideoFileSource.h"

#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <propidl.h>

#include <cmath>
#include <cstring>

#include "SharedMemoryContract.h"

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mf.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")

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

const BYTE* RowPtr(const BYTE* data, LONG stride, UINT h, UINT y)
{
    if (stride >= 0) return data + (size_t)y * (size_t)stride;
    return data + (size_t)(h - 1 - y) * (size_t)(-stride);
}

void LetterboxNearest(const BYTE* src, UINT sw, UINT sh, LONG sstride, BYTE* dst)
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
        BYTE* drow = dst + (size_t)(y0 + y) * vcam::VCamStride + (size_t)x0 * 4;
        for (int x = 0; x < dw; x++) {
            UINT sx = (UINT)((int64_t)x * sw / dw);
            if (sx >= sw) sx = sw - 1;
            const BYTE* p = row + (size_t)sx * 4;
            BYTE* q = drow + (size_t)x * 4;
            q[0] = p[0]; q[1] = p[1]; q[2] = p[2]; q[3] = p[3];
        }
    }
}

// Preserves aspect ratio: fits the source into 1280x720, centers it and fills
// the rest with black (letterbox / pillarbox). Fixed-point 16.16 bilinear.
void LetterboxBilinear(const BYTE* src, UINT sw, UINT sh, LONG sstride, BYTE* dst)
{
    const UINT tw = vcam::VCamWidth, th = vcam::VCamHeight;
    memset(dst, 0, (size_t)vcam::VCamStride * th);
    if (sw == 0 || sh == 0) return;
    if (sw < 2 || sh < 2) {
        LetterboxNearest(src, sw, sh, sstride, dst);
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
        BYTE* drow = dst + (size_t)(y0 + y) * vcam::VCamStride + (size_t)x0 * 4;
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

void RenderToFrame(const BYTE* data, UINT w, UINT h, LONG stride, BYTE* dst)
{
    if (w == vcam::VCamWidth && h == vcam::VCamHeight &&
        stride == (LONG)vcam::VCamStride) {
        for (UINT y = 0; y < h; y++) {
            memcpy(dst + (size_t)y * vcam::VCamStride, RowPtr(data, stride, h, y), vcam::VCamStride);
        }
        return;
    }
    LetterboxBilinear(data, w, h, stride, dst);
}

struct VideoState {
    IMFSourceReader* reader = nullptr;
    DWORD streamIndex = 0;
    UINT outW = 0;
    UINT outH = 0;
    LONG stride = 0;
};

void CloseVideo(VideoState& v)
{
    if (v.reader) { v.reader->Release(); v.reader = nullptr; }
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
        IMFMediaType* mt = nullptr;
        if (FAILED(rdr->GetNativeMediaType(i, 0, &mt)) || !mt) break;
        GUID maj = GUID_NULL;
        mt->GetMajorType(&maj);
        mt->Release();
        if (maj == MFMediaType_Video) { vid = i; break; }
    }
    if (vid == MAXDWORD) { err = L"no video stream"; return false; }

    rdr->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE);
    rdr->SetStreamSelection(vid, TRUE);

    UINT32 srcW = 0, srcH = 0;
    IMFMediaType* pNat = nullptr;
    if (SUCCEEDED(rdr->GetNativeMediaType(vid, 0, &pNat)) && pNat) {
        UINT64 fs = 0;
        if (SUCCEEDED(pNat->GetUINT64(MF_MT_FRAME_SIZE, &fs))) {
            srcW = (UINT32)(fs >> 32);
            srcH = (UINT32)(fs & 0xFFFFFFFFu);
        }
        pNat->Release();
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
        IMFMediaType* pOut = nullptr;
        if (FAILED(MFCreateMediaType(&pOut)) || !pOut) continue;
        pOut->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        pOut->SetGUID(MF_MT_SUBTYPE, c.subtype);
        if (c.w && c.h) pOut->SetUINT64(MF_MT_FRAME_SIZE, ((UINT64)c.w << 32) | (UINT64)c.h);
        if (c.interlace) pOut->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        HRESULT hr = rdr->SetCurrentMediaType(vid, nullptr, pOut);
        pOut->Release();
        hrSet = hr;
        if (SUCCEEDED(hr)) { usedCand = c.name; break; }
        LogVideo(std::wstring(c.name) + L" -> " + HrHex(hr));
    }

    if (FAILED(hrSet) || !usedCand) {
        err = L"SetCurrentMediaType(RGB) failed: " + HrHex(hrSet);
        return false;
    }
    LogVideo(std::wstring(L"output format: ") + usedCand);

    IMFMediaType* pCur = nullptr;
    HRESULT hr = rdr->GetCurrentMediaType(vid, &pCur);
    if (FAILED(hr) || !pCur) { err = L"GetCurrentMediaType failed: " + HrHex(hr); return false; }
    UINT64 cur = 0;
    if (FAILED(pCur->GetUINT64(MF_MT_FRAME_SIZE, &cur))) cur = 0;
    LONG stride = 0;
    if (FAILED(pCur->GetUINT32(MF_MT_DEFAULT_STRIDE, (UINT32*)&stride))) stride = 0;
    pCur->Release();

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
        IMFAttributes* pAttr = nullptr;
        if (pass.attr) {
            if (FAILED(MFCreateAttributes(&pAttr, 1)) || !pAttr) pAttr = nullptr;
            else pAttr->SetUINT32(*pass.attr, TRUE);
        }
        IMFSourceReader* rdr = nullptr;
        HRESULT hr = MFCreateSourceReaderFromURL(path.c_str(), pAttr, &rdr);
        if (pAttr) pAttr->Release();
        if (FAILED(hr)) {
            if (rdr) rdr->Release();
            err = L"MFCreateSourceReaderFromURL failed: " + HrHex(hr);
            return false;
        }

        VideoState tmp;
        std::wstring e2;
        if (ConfigureReader(rdr, tmp, e2)) {
            tmp.reader = rdr;
            out = tmp;
            LogVideo(std::wstring(L"reader mode: ") + pass.name);
            return true;
        }
        err = e2;
        if (rdr) rdr->Release();
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

VideoFileSource::VideoFileSource()
{
    InitializeCriticalSection(&cs_);
}

VideoFileSource::~VideoFileSource()
{
    Close();
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

    Close();

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

    stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!stopEvent_) {
        err = L"CreateEventW failed: " + std::to_wstring(GetLastError());
        frame_.clear();
        return false;
    }
    thread_ = CreateThread(nullptr, 0, ThreadProc, this, 0, nullptr);
    if (!thread_) {
        err = L"CreateThread failed: " + std::to_wstring(GetLastError());
        CloseHandle(stopEvent_);
        stopEvent_ = nullptr;
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
        if (stride == (int)vcam::VCamStride) {
            memcpy(bgrx, frame_.data(), vcam::VCamFrameSize);
        } else {
            for (UINT32 y = 0; y < vcam::VCamHeight; y++) {
                memcpy(bgrx + (SIZE_T)y * (size_t)stride,
                       frame_.data() + (SIZE_T)y * vcam::VCamStride, vcam::VCamStride);
            }
        }
    }
    LeaveCriticalSection(&cs_);
    return ready;
}

void VideoFileSource::Close()
{
    if (stopEvent_) SetEvent(stopEvent_);
    if (thread_) {
        WaitForSingleObject(thread_, 3000);
        CloseHandle(thread_);
        thread_ = nullptr;
    }
    if (stopEvent_) { CloseHandle(stopEvent_); stopEvent_ = nullptr; }

    EnterCriticalSection(&cs_);
    open_ = false;
    frameReady_ = false;
    failed_ = false;
    failReason_.clear();
    LeaveCriticalSection(&cs_);

    frame_.clear();
    frame_.shrink_to_fit();
    cfg_ = SourceConfig();
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
        IMFMediaBuffer* buf = nullptr;
        DWORD actualStream = 0;
        DWORD flags = 0;
        LONGLONG ts = 0;
        IMFSample* sample = nullptr;
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
                    sample->Release();
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
                buf->Release();
                buf = nullptr;
            }
            sample->Release();
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
