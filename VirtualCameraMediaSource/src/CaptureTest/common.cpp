#include "CaptureTestPCH.h"
#include "CaptureTestTypes.h"
#include "CaptureTestCommon.h"

static HANDLE g_logFile = INVALID_HANDLE_VALUE;

void LogW(const wchar_t* fmt, ...)
{
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t body[8192];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf_s(body, _countof(body), _TRUNCATE, fmt, ap);
    va_end(ap);
    wprintf(L"%02d:%02d:%02d.%03d %s\n", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, body);
    fflush(stdout);
    if (g_logFile != INVALID_HANDLE_VALUE) {
        wchar_t line[16384];
        swprintf_s(line, L"%02d:%02d:%02d.%03d %s\n", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, body);
        char mb[16384];
        int n = WideCharToMultiByte(CP_UTF8, 0, line, -1, mb, sizeof(mb), nullptr, nullptr);
        if (n > 1) {
            DWORD written = 0;
            WriteFile(g_logFile, mb, n - 1, &written, nullptr);
        }
    }
}

void SetLogFile(const wchar_t* path)
{
    if (g_logFile != INVALID_HANDLE_VALUE) CloseHandle(g_logFile);
    g_logFile = INVALID_HANDLE_VALUE;
    if (path) {
        g_logFile = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (g_logFile == INVALID_HANDLE_VALUE) LogW(L"cannot open log file %s", path);
    }
}

static void GuidToW(REFGUID g, wchar_t* out, size_t cch)
{
    swprintf_s(out, cch, L"{%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
        g.Data1, g.Data2, g.Data3,
        g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3],
        g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
}

bool Sha256Hex(const BYTE* data, DWORD len, wchar_t* out, size_t cch)
{
    HCRYPTPROV hProv = 0;
    HCRYPTHASH hHash = 0;
    BYTE hash[32];
    DWORD hashLen = 32;
    if (!CryptAcquireContextW(&hProv, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT)) return false;
    bool ok = false;
    if (CryptCreateHash(hProv, CALG_SHA_256, 0, 0, &hHash)) {
        if (CryptHashData(hHash, data, len, 0) && CryptGetHashParam(hHash, HP_HASHVAL, hash, &hashLen, 0)) {
            wchar_t* p = out;
            for (DWORD i = 0; i < 32 && (size_t)(p - out) < cch - 3; ++i)
                p += swprintf_s(p, cch - (p - out), L"%02x", hash[i]);
            ok = true;
        }
        CryptDestroyHash(hHash);
    }
    CryptReleaseContext(hProv, 0);
    return ok;
}

PixelStats ComputePixelStats(const BYTE* pBits, DWORD curLen, int w, int h, int stride, double threshold)
{
    PixelStats st;
    if (!pBits || w <= 0 || h <= 0 || stride <= 0) return st;
    double sum = 0, sumSq = 0;
    int count = 0;
    int mn = 255, mx = 0;
    for (int y = 0; y < h; y += 8) {
        const BYTE* row = pBits + (SIZE_T)y * stride;
        for (int x = 0; x < w; x += 8) {
            int b = row[x * 4], g = row[x * 4 + 1], r = row[x * 4 + 2];
            int lum = LuminanceBt601(r, g, b);
            sum += lum;
            sumSq += (double)lum * lum;
            if (lum < mn) mn = lum;
            if (lum > mx) mx = lum;
            ++count;
        }
    }
    if (count > 0) {
        st.mean = sum / count;
        double var = sumSq / count - st.mean * st.mean;
        st.stddev = var > 0 ? sqrt(var) : 0;
        st.min = mn;
        st.max = mx;
    }
    return st;
}

static int Clip255(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }

void SaveBMP(const wchar_t* filename, const BYTE* rgb32Data, int width, int height, int stride)
{
    FILE* f = _wfopen(filename, L"wb");
    if (!f) { LogW(L"SaveBMP: cannot open %s", filename); return; }
    int paddedRow = (width * 3 + 3) & ~3;
    DWORD fileSize = 14 + 40 + paddedRow * height;
    std::array<unsigned char, 14> fileHeader = {
        'B', 'M',
        (unsigned char)(fileSize & 0xFF), (unsigned char)((fileSize >> 8) & 0xFF),
        (unsigned char)((fileSize >> 16) & 0xFF), (unsigned char)((fileSize >> 24) & 0xFF),
        0, 0, 0, 0, 54, 0, 0, 0
    };
    fwrite(fileHeader.data(), 1, fileHeader.size(), f);
    std::array<unsigned char, 40> dibHeader = {
        40, 0, 0, 0,
        (unsigned char)(width & 0xFF), (unsigned char)((width >> 8) & 0xFF),
        (unsigned char)((width >> 16) & 0xFF), (unsigned char)((width >> 24) & 0xFF),
        (unsigned char)(height & 0xFF), (unsigned char)((height >> 8) & 0xFF),
        (unsigned char)((height >> 16) & 0xFF), (unsigned char)((height >> 24) & 0xFF),
        1, 0, 24, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
    };
    fwrite(dibHeader.data(), 1, dibHeader.size(), f);
    unsigned char* dstRow = (unsigned char*)malloc(paddedRow);
    for (int y = height - 1; y >= 0; --y) {
        const BYTE* srcRow = rgb32Data + (SIZE_T)y * stride;
        for (int x = 0; x < width; ++x) {
            dstRow[x * 3] = srcRow[x * 4];
            dstRow[x * 3 + 1] = srcRow[x * 4 + 1];
            dstRow[x * 3 + 2] = srcRow[x * 4 + 2];
        }
        fwrite(dstRow, 1, paddedRow, f);
    }
    free(dstRow);
    fclose(f);
    LogW(L"Saved: %s", filename);
}

static void SaveNv12AsBmp(const wchar_t* filename, const BYTE* pNv12, int w, int h)
{
    BYTE* tmp = (BYTE*)malloc((SIZE_T)w * h * 4);
    if (!tmp) { LogW(L"SaveNv12AsBmp: malloc failed"); return; }
    const BYTE* pY = pNv12;
    const BYTE* pUV = pNv12 + (SIZE_T)w * h;
    for (int y = 0; y < h; ++y) {
        const BYTE* yRow = pY + (SIZE_T)y * w;
        const BYTE* uvRow = pUV + (SIZE_T)(y >> 1) * w;
        BYTE* dst = tmp + (SIZE_T)y * w * 4;
        for (int x = 0; x < w; ++x) {
            const int c = yRow[x] - 16;
            const int d = uvRow[x & ~1] - 128;
            const int e = uvRow[(x & ~1) + 1] - 128;
            const int r = (298 * c + 409 * e + 128) >> 8;
            const int g = (298 * c - 100 * d - 208 * e + 128) >> 8;
            const int b = (298 * c + 516 * d + 128) >> 8;
            dst[x * 4] = (BYTE)Clip255(b);
            dst[x * 4 + 1] = (BYTE)Clip255(g);
            dst[x * 4 + 2] = (BYTE)Clip255(r);
            dst[x * 4 + 3] = 0xFF;
        }
    }
    SaveBMP(filename, tmp, w, h, w * 4);
    free(tmp);
}

ModeArgs ParseModeArgs(int argc, wchar_t** argv, int start, int defFrames, int defInterval)
{
    ModeArgs a;
    a.frames = defFrames;
    a.intervalMs = defInterval;
    int i = start;
    int pos = 0;
    while (i < argc) {
        const wchar_t* s = argv[i];
        if (!_wcsicmp(s, L"--frames") && i + 1 < argc) { a.frames = _wtoi(argv[++i]); }
        else if (!_wcsicmp(s, L"--interval") && i + 1 < argc) { a.intervalMs = _wtoi(argv[++i]); }
        else if (!_wcsicmp(s, L"--duration") && i + 1 < argc) { a.durationSec = _wtoi(argv[++i]); }
        else if (!_wcsicmp(s, L"--threshold") && i + 1 < argc) { a.threshold = _wtof(argv[++i]); }
        else if (!_wcsicmp(s, L"--allow-black") && i + 1 < argc) { a.allowBlack = _wtoi(argv[++i]); }
        else if (!_wcsicmp(s, L"--prefix") && i + 1 < argc) { a.prefix = argv[++i]; }
        else if (!_wcsicmp(s, L"--log") && i + 1 < argc) { a.logFile = argv[++i]; }
        else if (!_wcsicmp(s, L"--settings") && i + 1 < argc) { a.settingsFile = argv[++i]; }
        else if (!_wcsicmp(s, L"--json")) { a.json = true; }
        else if (!_wcsicmp(s, L"--nv12")) { a.nv12 = true; }
        else if (!_wcsicmp(s, L"--save-black")) { a.saveBlack = true; }
        else if (s[0] != L'-') {
            if (pos == 0) a.nameFilter = s;
            else if (pos == 1) a.reqW = (UINT32)_wtoi(s);
            else if (pos == 2) a.reqH = (UINT32)_wtoi(s);
            ++pos;
        }
        ++i;
    }
    return a;
}

std::wstring JsonEscape(const std::wstring& s)
{
    std::wstring out;
    for (wchar_t c : s) {
        if (c == L'"' || c == L'\\') { out += L'\\'; out += c; }
        else if (c == L'\n') out += L"\\n";
        else if (c == L'\r') out += L"\\r";
        else if (c == L'\t') out += L"\\t";
        else out += c;
    }
    return out;
}

ULONGLONG FileMtimeMs(const wchar_t* path)
{
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (!GetFileAttributesExW(path, GetFileExInfoStandard, &fad)) return 0;
    ULONGLONG t = ((ULONGLONG)fad.ftLastWriteTime.dwHighDateTime << 32) | fad.ftLastWriteTime.dwLowDateTime;
    return t / 10000ULL - 11644473600000ULL;
}

double Percentile(std::vector<ULONGLONG>& v, double p)
{
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    size_t idx = (size_t)(p * (v.size() - 1));
    return (double)v[idx];
}

int FindDevice(const wchar_t* nameFilter, IMFActivate** ppDevices, UINT32 count)
{
    int match = -1;
    int firstUnnamed = -1;
    for (UINT32 i = 0; i < count; ++i) {
        wchar_t name[256] = L"(none)";
        bool hasName = false;
        PROPVARIANT vt;
        PropVariantInit(&vt);
        if (SUCCEEDED(ppDevices[i]->GetItem(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &vt))
            && vt.vt == VT_BSTR && vt.bstrVal) {
            hasName = true;
            wcsncpy_s(name, _countof(name), vt.bstrVal, 255);
        }
        PropVariantClear(&vt);
        if (match < 0 && hasName && wcsstr(name, nameFilter) != nullptr) match = (int)i;
        else if (firstUnnamed < 0 && !hasName) firstUnnamed = (int)i;
    }
    {
        bool allDigits = nameFilter[0] != L'\0';
        for (const wchar_t* c = nameFilter; *c; ++c)
            if (*c < L'0' || *c > L'9') { allDigits = false; break; }
        if (allDigits) {
            int forced = _wtoi(nameFilter);
            if (forced >= 0 && forced < (int)count) match = forced;
        }
    }
    if (match < 0 && firstUnnamed >= 0) match = firstUnnamed;
    return match;
}

int OpenDeviceSession(const wchar_t* nameFilter, UINT32 reqW, UINT32 reqH, bool wantNv12,
                      MfSession& session, ATL::CComPtr<IMFActivate>& pAct,
                      ATL::CComPtr<IMFMediaSource>& pSource,
                      ATL::CComPtr<IMFMediaStream>& pStream, Stream0Info& s0)
{
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr)) { LogW(L"CoInitializeEx failed: 0x%08X", hr); return 2; }
    session.comOk = true;
    hr = MFStartup(MF_VERSION);
    if (FAILED(hr)) { LogW(L"MFStartup failed: 0x%08X", hr); return 2; }
    session.mfOk = true;

    IMFActivate** ppDevices = nullptr;
    UINT32 count = 0;
    ATL::CComPtr<IMFAttributes> pEnumAttr;
    hr = MFCreateAttributes(&pEnumAttr, 1);
    if (SUCCEEDED(hr)) {
        pEnumAttr->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
                           MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
        hr = MFEnumDeviceSources(pEnumAttr, &ppDevices, &count);
        pEnumAttr = nullptr;
    }
    if (FAILED(hr)) { LogW(L"MFEnumDeviceSources: hr=0x%08X", hr); return 2; }

    int match = FindDevice(nameFilter, ppDevices, count);
    if (match < 0) {
        LogW(L"OpenDeviceSession: NO DEVICE matching '%s'", nameFilter);
        CoTaskMemFree(ppDevices);
        return 2;
    }

    pAct.Attach(ppDevices[match]);
    pAct.p->AddRef();
    CoTaskMemFree(ppDevices);

    hr = pAct->ActivateObject(IID_IMFMediaSource, (void**)&pSource);
    if (FAILED(hr)) { LogW(L"ActivateObject hr=0x%08X", hr); return 2; }

    ATL::CComPtr<IMFPresentationDescriptor> pPD;
    hr = pSource->CreatePresentationDescriptor(&pPD);
    if (FAILED(hr)) { LogW(L"CreatePD hr=0x%08X", hr); pSource->Shutdown(); return 2; }

    BOOL bSel = FALSE;
    ATL::CComPtr<IMFStreamDescriptor> pSD;
    if (FAILED(pPD->GetStreamDescriptorByIndex(0, &bSel, &pSD)) || !pSD) {
        LogW(L"GetStreamDescriptorByIndex(0) failed");
        pSource->Shutdown();
        return 2;
    }
    ATL::CComPtr<IMFMediaTypeHandler> pMTH;
    if (FAILED(pSD->GetMediaTypeHandler(&pMTH)) || !pMTH) {
        LogW(L"GetMediaTypeHandler failed");
        pSource->Shutdown();
        return 2;
    }

    DWORD mtCount = 0;
    pMTH->GetMediaTypeCount(&mtCount);
    bool found = false;
    for (DWORD m = 0; m < mtCount && !found; ++m) {
        ATL::CComPtr<IMFMediaType> pMT;
        if (FAILED(pMTH->GetMediaTypeByIndex(m, &pMT)) || !pMT) continue;
        GUID sub = {};
        UINT32 w = 0, h = 0;
        pMT->GetGUID(MF_MT_SUBTYPE, &sub);
        MFGetAttributeSize(pMT, MF_MT_FRAME_SIZE, &w, &h);
        bool isNv12 = (sub == MFVideoFormat_NV12);
        bool isRgb32 = (sub == MFVideoFormat_RGB32);
        if ((wantNv12 && isNv12) || (!wantNv12 && isRgb32)) {
            if (reqW == 0 || reqH == 0 || (w == reqW && h == reqH)) {
                if (SUCCEEDED(pMTH->SetCurrentMediaType(pMT))) {
                    s0.sub = sub;
                    s0.dwWidth = w;
                    s0.dwHeight = h;
                    s0.haveImage = true;
                    found = true;
                }
            }
        }
    }
    if (!found) {
        LogW(L"no matching media type (wantNv12=%d req=%ux%u)", (int)wantNv12, reqW, reqH);
        pSource->Shutdown();
        return 2;
    }

    PROPVARIANT vtStart;
    vtStart.vt = VT_EMPTY;
    hr = pSource->Start(pPD, nullptr, &vtStart);
    if (FAILED(hr)) { LogW(L"Start hr=0x%08X", hr); pSource->Shutdown(); return 2; }

    ULONGLONG t0 = GetTickCount64();
    while (!pStream && (GetTickCount64() - t0) < 5000) {
        ATL::CComPtr<IMFMediaEvent> pEvent;
        hr = pSource->GetEvent(MF_EVENT_FLAG_NO_WAIT, &pEvent);
        if (FAILED(hr)) { Sleep(20); continue; }
        MediaEventType met;
        pEvent->GetType(&met);
        if (met == MENewStream) {
            PROPVARIANT vt;
            vt.vt = VT_UNKNOWN;
            pEvent->GetValue(&vt);
            pStream = static_cast<IMFMediaStream*>(vt.punkVal);
            PropVariantClear(&vt);
        }
        pEvent = nullptr;
    }
    if (!pStream) {
        LogW(L"no media stream within 5s");
        pSource->Shutdown();
        return 2;
    }
    return 0;
}

void CloseDeviceSession(ATL::CComPtr<IMFMediaStream>& pStream,
                        ATL::CComPtr<IMFMediaSource>& pSource,
                        ATL::CComPtr<IMFActivate>& pAct)
{
    if (pStream) { pStream = nullptr; }
    if (pSource) { pSource->Shutdown(); pSource = nullptr; }
    if (pAct) { pAct = nullptr; }
}

int RunCaptureSession(const CapOptions& opt, FrameCallback cb, void* ctx,
                      std::vector<ULONGLONG>* requestMsOut,
                      std::vector<ULONGLONG>* gapsOut)
{
    MfSession session;
    ATL::CComPtr<IMFActivate> pAct;
    ATL::CComPtr<IMFMediaSource> pSource;
    ATL::CComPtr<IMFMediaStream> pStream;
    Stream0Info s0;
    int rc = OpenDeviceSession(opt.nameFilter, opt.reqW, opt.reqH, opt.wantNv12,
                               session, pAct, pSource, pStream, s0);
    if (rc != 0) return rc;

    int received = 0;
    ULONGLONG prevT = 0;
    ULONGLONG deadline = GetTickCount64() + opt.deadlineMs;
    while (received < opt.numFrames && GetTickCount64() < deadline) {
        ATL::CComPtr<CToken> pToken;
        pToken.Attach(new CToken());
        if (FAILED(pStream->RequestSample(pToken))) { pToken = nullptr; break; }
        bool got = false;
        ULONGLONG st0 = GetTickCount64();
        while (!got && (GetTickCount64() - st0) < 2000) {
            ATL::CComPtr<IMFMediaEvent> pEvent;
            HRESULT hr = pStream->GetEvent(MF_EVENT_FLAG_NO_WAIT, &pEvent);
            if (FAILED(hr)) { Sleep(20); continue; }
            MediaEventType met;
            pEvent->GetType(&met);
            if (met == MEMediaSample) {
                PROPVARIANT vt;
                vt.vt = VT_UNKNOWN;
                pEvent->GetValue(&vt);
                ATL::CComPtr<IMFSample> pSample;
                pSample = static_cast<IMFSample*>(vt.punkVal);
                PropVariantClear(&vt);
                ATL::CComPtr<IMFMediaBuffer> pBuffer;
                if (SUCCEEDED(pSample->ConvertToContiguousBuffer(&pBuffer)) && pBuffer) {
                    BYTE* pBits = nullptr;
                    DWORD maxLen = 0, curLen = 0;
                    if (SUCCEEDED(pBuffer->Lock(&pBits, &maxLen, &curLen)) && pBits) {
                        FrameCtx fc;
                        fc.index = received;
                        fc.curLen = curLen;
                        fc.requestMs = GetTickCount64() - st0;
                        fc.tMs = GetTickCount64();
                        fc.pBits = pBits;
                        fc.s0 = s0;
                        cb(ctx, fc);
                        pBuffer->Unlock();
                        ++received;
                        if (prevT && gapsOut) gapsOut->push_back(fc.tMs - prevT);
                        prevT = fc.tMs;
                        if (requestMsOut) requestMsOut->push_back(fc.requestMs);
                        got = true;
                    } else {
                        pBuffer->Unlock();
                    }
                }
                pSample = nullptr;
                pEvent = nullptr;
                break;
            }
            pEvent = nullptr;
        }
        pToken = nullptr;
        if (opt.intervalMs > 0) Sleep(opt.intervalMs);
    }

    CloseDeviceSession(pStream, pSource, pAct);
    return received > 0 ? 0 : 1;
}