#include <windows.h>
#include <sddl.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <propidl.h>
#include <atlbase.h>
#include <cstdio>
#include <cwchar>
#include <cmath>
#include <string>
#include <vector>
#include <memory>
#include <new>
#include "../Common/SharedMemoryContract.h"
#include "../Common/MappedViewOfFilePtr.h"
#include "../Common/CriticalSectionGuard.h"
#include "../Common/WinUtil.h"

using vcam::HrHex;

#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mf.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")

struct Settings {
    std::wstring mediaPath;
    std::wstring mediaMode; // "static" | "video"
};

// Frame produced by the decode thread; the 30 FPS writer publishes it.
static std::unique_ptr<BYTE[]> g_pFrame;
static CRITICAL_SECTION g_frameCs;   // guards g_pFrame
static CRITICAL_SECTION g_targetCs;  // guards g_targetPath / g_targetMode
static HANDLE g_hStopEvent = nullptr;

static std::wstring g_cliSource;     // argv[1] fallback source
static std::wstring g_targetPath;    // desired video path (settings mediaPath, else CLI)
static std::wstring g_targetMode;    // mediaMode from settings

static std::wstring SettingsDirPath()
{
    wchar_t appdata[MAX_PATH] = {};
    DWORD n = GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return std::wstring();
    return std::wstring(appdata) + L"\\VCam";
}

static std::wstring SettingsFilePath()
{
    std::wstring dir = SettingsDirPath();
    if (dir.empty()) return std::wstring();
    return dir + L"\\settings.json";
}

static bool ReadUtf8File(const std::wstring& path, std::string& out)
{
    HANDLE raw = CreateFileW(path.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (raw == INVALID_HANDLE_VALUE) return false;
    ATL::CHandle h(raw);
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart <= 0 || sz.QuadPart > 1000000) return false;
    out.resize((size_t)sz.QuadPart);
    DWORD read = 0;
    BOOL ok = ReadFile(h, &out[0], (DWORD)out.size(), &read, nullptr);
    return ok && read == out.size();
}

static std::wstring Utf8ToWide(const std::string& s)
{
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

// Minimal flat-JSON string extractor: finds "key" then the quoted value after ':'.
// Handles standard escapes (\\ \" \/ \n \r \t \b \f).
static bool JsonGetString(const std::string& json, const char* key, std::wstring& value)
{
    std::string k = std::string("\"") + key + "\"";
    size_t p = json.find(k);
    if (p == std::string::npos) return false;
    p = json.find(':', p + k.size());
    if (p == std::string::npos) return false;
    p++;
    while (p < json.size() && (json[p] == ' ' || json[p] == '\t' || json[p] == '\r' || json[p] == '\n')) p++;
    if (p >= json.size() || json[p] != '"') return false;
    std::string val;
    for (p++; p < json.size() && json[p] != '"'; p++) {
        if (json[p] == '\\' && p + 1 < json.size()) {
            char c = json[++p];
            switch (c) {
                case 'n': val += '\n'; break;
                case 'r': val += '\r'; break;
                case 't': val += '\t'; break;
                case 'b': val += '\b'; break;
                case 'f': val += '\f'; break;
                case 'u':
                    if (p + 4 < json.size()) p += 4;
                    val += '?';
                    break;
                default: val += c; break;
            }
        } else {
            val += json[p];
        }
    }
    if (p >= json.size()) return false;
    value = Utf8ToWide(val);
    return true;
}

// mediaPath wins when present; argv[1] is the fallback source.
static std::wstring DesiredSource(const Settings& s)
{
    if (!s.mediaPath.empty()) return s.mediaPath;
    return g_cliSource;
}

static bool LoadSettings(Settings& s)
{
    std::wstring path = SettingsFilePath();
    if (path.empty()) return false;
    std::string json;
    if (!ReadUtf8File(path, json)) return false;
    JsonGetString(json, "mediaPath", s.mediaPath);
    JsonGetString(json, "mediaMode", s.mediaMode);
    return true;
}

static void LogMediaMode(const std::wstring& mode)
{
    if (mode == L"video") {
        wprintf(L"[settings] mediaMode=video (normal for VideoProducer)\n");
    } else if (mode.empty()) {
        wprintf(L"[settings] mediaMode not set (expected \"video\"), continuing anyway\n");
    } else {
        wprintf(L"[settings] WARNING: mediaMode=\"%s\" but VideoProducer expects \"video\" - continuing anyway\n",
                mode.c_str());
    }
    fflush(stdout);
}

// ---------------------------------------------------------------------------
// Frame rendering: letterbox bilinear resample into 1280x720 BGRX (stride 5120)
// ---------------------------------------------------------------------------

// data points at the start of the contiguous buffer; stride > 0 = top-down.
static inline const BYTE* RowPtr(const BYTE* data, LONG stride, UINT h, UINT y)
{
    if (stride >= 0) return data + (size_t)y * (size_t)stride;
    return data + (size_t)(h - 1 - y) * (size_t)(-stride);
}

static void LetterboxNearest(const BYTE* src, UINT sw, UINT sh, LONG sstride, BYTE* dst)
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
static void LetterboxBilinear(const BYTE* src, UINT sw, UINT sh, LONG sstride, BYTE* dst)
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

static void RenderToFrame(const BYTE* data, UINT w, UINT h, LONG stride, BYTE* dst)
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

// ---------------------------------------------------------------------------
// Media Foundation source reader
// ---------------------------------------------------------------------------

struct VideoState {
    ATL::CComPtr<IMFSourceReader> reader;
    DWORD streamIndex = 0;
    UINT outW = 0;
    UINT outH = 0;
    LONG stride = 0;
};

static void CloseVideo(VideoState& v)
{
    v.reader = nullptr;
    v.outW = v.outH = 0;
    v.stride = 0;
}

// Selects the first video stream and negotiates an RGB32 output.
// Asks for a 1280x720 output only when that cannot change the aspect ratio;
// otherwise decodes at the native size (the caller letterboxes it itself).
static bool ConfigureReader(IMFSourceReader* rdr, VideoState& out, std::wstring& err)
{
    // IMFSourceReader exposes no stream count: probe until native types run out.
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

    // Negotiate an RGB output. MF rejects a bare "RGB32" type as inconsistent
    // (MF_E_INVALIDMEDIATYPE), so a frame size has to be part of the request;
    // try the shapes we actually want, in order.
    struct Candidate {
        GUID subtype;
        UINT32 w;   // 0 = leave the frame size unspecified
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
            c.w == vcam::VCamWidth && c.h == vcam::VCamHeight) continue; // would stretch
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
        wprintf(L"[video]   %s -> %s\n", c.name, HrHex(hr).c_str());
    }

    if (FAILED(hrSet) || !usedCand) {
        err = L"SetCurrentMediaType(RGB) failed: " + HrHex(hrSet);
        return false;
    }
    wprintf(L"[video] output format: %s\n", usedCand);
    fflush(stdout);

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

// Opens the source reader; retries with reader attributes that enable the
// video processor (RGB conversion is not accepted without it on some files).
static bool OpenVideo(const std::wstring& path, VideoState& out, std::wstring& err)
{
    if (path.empty()) { err = L"empty media path"; return false; }

    // RGB output is rejected by a plain reader on most files
    // (MF_E_INVALIDMEDIATYPE), so the video processor comes first.
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
            wprintf(L"[video] reader mode: %s\n", pass.name);
            fflush(stdout);
            return true;
        }
        err = e2;
        rdr = nullptr;
        wprintf(L"[video] reader mode %s failed: %s\n", pass.name, e2.c_str());
        fflush(stdout);
    }
    return false;
}

// Decode thread: reads samples, paces them along the clip timeline (frame
// holding), letterboxes the current frame into g_pFrame under g_frameCs and
// seeks back to 0 on end-of-stream (loop).
static DWORD WINAPI DecodeThread(LPVOID)
{
    HRESULT hrCo = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool coInited = SUCCEEDED(hrCo);
    HRESULT hrMf = MFStartup(MF_VERSION, MFSTARTUP_FULL);
    if (FAILED(hrMf)) {
        wprintf(L"[video] MFStartup failed: %s\n", HrHex(hrMf).c_str());
        fflush(stdout);
    }

    VideoState vs;
    std::wstring activePath;
    std::wstring failedPath;
    bool firstFrameLogged = false;

    ULONGLONG wallStart = GetTickCount64();
    LONGLONG baseMs = 0; // timeline base, ms relative to wallStart

    while (WaitForSingleObject(g_hStopEvent, 0) != WAIT_OBJECT_0) {
        std::wstring want;
        {
            vcam::CsGuard guard(&g_targetCs);
            want = g_targetPath;
        }

        if (want != activePath) {
            VideoState next;
            std::wstring err;
            if (SUCCEEDED(hrMf) && OpenVideo(want, next, err)) {
                CloseVideo(vs);
                vs = std::move(next);
                activePath = want;
                wallStart = GetTickCount64();
                baseMs = 0;
                firstFrameLogged = false;
                wprintf(L"[video] opened: %s (%ux%u stride=%ld)\n", want.c_str(),
                        vs.outW, vs.outH, vs.stride);
            } else {
                if (want != failedPath) {
                    if (SUCCEEDED(hrMf)) {
                        wprintf(L"[video] failed to open: %s (%s)\n", want.c_str(), err.c_str());
                    } else {
                        wprintf(L"[video] Media Foundation unavailable: %s\n", HrHex(hrMf).c_str());
                    }
                    fflush(stdout);
                    failedPath = want;
                }
                // Keep playing the previous clip; when there is none, retry the
                // same path silently until settings change again.
                activePath = vs.reader ? want : std::wstring();
            }
            fflush(stdout);
        }

        if (!vs.reader) {
            if (WaitForSingleObject(g_hStopEvent, 200) != WAIT_TIMEOUT) break;
            continue;
        }

        ATL::CComPtr<IMFMediaBuffer> buf;
        DWORD actualStream = 0;
        DWORD flags = 0;
        LONGLONG ts = 0;
        ATL::CComPtr<IMFSample> sample;
        HRESULT hr = vs.reader->ReadSample(vs.streamIndex, 0, &actualStream, &flags, &ts, &sample);

        if (FAILED(hr)) {
            wprintf(L"[video] ReadSample failed: %s - reopening\n", HrHex(hr).c_str());
            fflush(stdout);
            CloseVideo(vs);
            activePath.clear(); // reopen on the next iteration
            if (WaitForSingleObject(g_hStopEvent, 500) != WAIT_TIMEOUT) break;
            continue;
        }

        if (sample) {
            // Pace playback by the sample timestamp (frame holding between
            // video frames and the 30 FPS writer cadence).
            ULONGLONG now = GetTickCount64() - wallStart;
            LONGLONG target = baseMs + ts / 10000;
            if ((LONGLONG)now > target + 250) {
                baseMs = (LONGLONG)now - ts / 10000; // too late: resync
                target = now;
            }
            if (target > (LONGLONG)now) {
                DWORD wait = (DWORD)(target - now);
                if (WaitForSingleObject(g_hStopEvent, wait) != WAIT_TIMEOUT) {
                    sample = nullptr;
                    break;
                }
            }

            if (SUCCEEDED(sample->ConvertToContiguousBuffer(&buf)) && buf) {
                BYTE* data = nullptr;
                DWORD maxLen = 0, curLen = 0;
                if (SUCCEEDED(buf->Lock(&data, &maxLen, &curLen)) && data) {
                    {
                        vcam::CsGuard guard(&g_frameCs);
                        RenderToFrame(data, vs.outW, vs.outH, vs.stride, g_pFrame.get());
                    }
                    buf->Unlock();
                    if (!firstFrameLogged) {
                        wprintf(L"[video] first frame rendered (%ux%u)\n", vs.outW, vs.outH);
                        fflush(stdout);
                        firstFrameLogged = true;
                    }
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
            if (SUCCEEDED(hrSeek)) {
                // Seamless loop: the next sample (ts=0) is behind the current
                // wall-clock position, so the resync branch restarts the
                // timeline without skipping any wait.
                wprintf(L"[video] end of stream - looping\n");
                fflush(stdout);
            } else {
                wprintf(L"[video] SetPosition(0) failed: %s - reopening\n", HrHex(hrSeek).c_str());
                fflush(stdout);
                CloseVideo(vs);
                activePath.clear();
                if (WaitForSingleObject(g_hStopEvent, 500) != WAIT_TIMEOUT) break;
            }
        }
    }

    CloseVideo(vs);
    if (coInited) CoUninitialize();
    return 0;
}

// Polls settings.json every 500ms; on mediaPath/mediaMode change switches the
// video source without restarting the process.
static DWORD WINAPI SettingsWatcherThread(LPVOID)
{
    std::wstring path = SettingsFilePath();
    ULARGE_INTEGER lastWrite = {};
    for (;;) {
        if (WaitForSingleObject(g_hStopEvent, 500) != WAIT_TIMEOUT) break;

        WIN32_FILE_ATTRIBUTE_DATA fad;
        if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad)) {
            lastWrite.QuadPart = 0;
            continue;
        }
        ULARGE_INTEGER ft;
        ft.LowPart = fad.ftLastWriteTime.dwLowDateTime;
        ft.HighPart = fad.ftLastWriteTime.dwHighDateTime;
        if (ft.QuadPart == lastWrite.QuadPart) continue;

        Sleep(200); // debounce: let the writer finish
        lastWrite = ft;

        Settings s;
        LoadSettings(s);
        std::wstring want = DesiredSource(s);

        bool modeChanged = (s.mediaMode != g_targetMode);
        bool pathChanged = !want.empty() && want != g_targetPath;

        if (modeChanged) {
            g_targetMode = s.mediaMode;
            LogMediaMode(s.mediaMode);
        }
        if (pathChanged) {
            {
                vcam::CsGuard guard(&g_targetCs);
                g_targetPath = want;
            }
            wprintf(L"[settings] mediaPath -> %s\n", want.c_str());
            fflush(stdout);
        }
    }
    return 0;
}

int wmain(int argc, wchar_t* argv[])
{
    Settings settings;
    bool haveSettings = LoadSettings(settings);

    if (argc >= 2) g_cliSource = argv[1]; // fallback when settings has no mediaPath
    std::wstring initial = DesiredSource(settings);

    if (initial.empty()) {
        wprintf(L"Usage: VideoProducer.exe [path_to_video]\n");
        wprintf(L"  Settings file: %s\n", SettingsFilePath().c_str());
        wprintf(L"  { \"mediaPath\": \"...\", \"mediaMode\": \"video\" }\n");
        return 1;
    }

    if (haveSettings) LogMediaMode(settings.mediaMode);
    wprintf(L"Loading video: %s\n", initial.c_str());
    fflush(stdout);

    std::unique_ptr<BYTE[]> pFrame;
    try { pFrame = std::make_unique_for_overwrite<BYTE[]>(vcam::VCamFrameSize); }
    catch (const std::bad_alloc&) { pFrame.reset(); }
    if (!pFrame) {
        wprintf(L"Out of memory!\n");
        return 1;
    }
    memset(pFrame.get(), 0, vcam::VCamFrameSize);

    InitializeCriticalSection(&g_frameCs);
    InitializeCriticalSection(&g_targetCs);
    g_pFrame = std::move(pFrame);
    g_targetPath = initial;
    g_targetMode = settings.mediaMode;
    g_hStopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);

    PSECURITY_DESCRIPTOR pSecDesc = nullptr;
    ConvertStringSecurityDescriptorToSecurityDescriptorW(vcam::VCamDacSddl, SDDL_REVISION_1, &pSecDesc, nullptr);
    SECURITY_ATTRIBUTES sa = { sizeof(sa), pSecDesc, FALSE };

    SIZE_T totalSize = sizeof(vcam::VCamSectionHeader) + (SIZE_T)vcam::VCamSlotCount * vcam::VCamFrameSize;
    ATL::CHandle hSection(CreateFileMappingW(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE,
        (DWORD)(totalSize >> 32), (DWORD)(totalSize & 0xFFFFFFFF), vcam::VCamSectionName));

    if (!hSection) {
        wprintf(L"CreateFileMappingW failed: %lu\n", GetLastError());
        LocalFree(pSecDesc);
        g_pFrame.reset();
        CloseHandle(g_hStopEvent);
        DeleteCriticalSection(&g_frameCs);
        DeleteCriticalSection(&g_targetCs);
        return 1;
    }

    ATL::CHandle hReadyEvent(CreateEventW(&sa, FALSE, FALSE, vcam::VCamReadyEventName));
    vcam::MappedViewOfFilePtr view(MapViewOfFileEx(hSection, FILE_MAP_ALL_ACCESS, 0, 0, totalSize, nullptr));
    BYTE* pBase = (BYTE*)view.Get();

    vcam::VCamSectionHeader* pHeader = (vcam::VCamSectionHeader*)pBase;
    pHeader->magic = vcam::VCamMagic;
    pHeader->version = vcam::VCamVersion;
    pHeader->width = vcam::VCamWidth;
    pHeader->height = vcam::VCamHeight;
    pHeader->stride = vcam::VCamStride;
    pHeader->pixelFormat = (UINT32)vcam::VCamPixelFormat::RGB32;
    pHeader->frameSize = vcam::VCamFrameSize;
    pHeader->slotCount = vcam::VCamSlotCount;
    pHeader->frameWriteIndex = 0;
    pHeader->seq = 0;

    ATL::CHandle hDecode(CreateThread(nullptr, 0, DecodeThread, nullptr, 0, nullptr));
    ATL::CHandle hWatcher(CreateThread(nullptr, 0, SettingsWatcherThread, nullptr, 0, nullptr));

    wprintf(L"Video Producer running @ 30 FPS. Press Ctrl+C or Esc to stop.\n");
    fflush(stdout);

    LARGE_INTEGER freq, counter, startTime;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&startTime);

    while (true) {
        if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) break;

        UINT32 slot = (pHeader->frameWriteIndex + 1) % vcam::VCamSlotCount;
        BYTE* pFrameSlot = pBase + sizeof(vcam::VCamSectionHeader) + (SIZE_T)slot * vcam::VCamFrameSize;

        _InterlockedExchangeAdd64((volatile LONGLONG*)&pHeader->seq, 1);
        {
            vcam::CsGuard guard(&g_frameCs);
            memcpy(pFrameSlot, g_pFrame.get(), vcam::VCamFrameSize);
        }
        pHeader->frameWriteIndex = slot;
        QueryPerformanceCounter(&counter);
        pHeader->lastFrameTime100ns = (UINT64)((counter.QuadPart - startTime.QuadPart) * 10000000 / freq.QuadPart);
        _InterlockedExchangeAdd64((volatile LONGLONG*)&pHeader->seq, 1);

        SetEvent(hReadyEvent);
        Sleep(33);
    }

    SetEvent(g_hStopEvent);
    if (hDecode) WaitForSingleObject(hDecode, 3000);
    if (hWatcher) WaitForSingleObject(hWatcher, 2000);
    hDecode.Close();
    hWatcher.Close();

    view.Close();
    hReadyEvent.Close();
    hSection.Close();
    LocalFree(pSecDesc);
    CloseHandle(g_hStopEvent);
    DeleteCriticalSection(&g_frameCs);
    DeleteCriticalSection(&g_targetCs);
    g_pFrame.reset();
    return 0;
}
