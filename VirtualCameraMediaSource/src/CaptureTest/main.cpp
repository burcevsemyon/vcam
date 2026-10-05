#define INITGUID
#include <windows.h>
#include <cguid.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfobjects.h>
#include <Mferror.h>
#include <atlbase.h>
#include <cstdio>
#include <array>
#include <cstring>
#include <cwchar>
#include <cstdarg>
#include <cstdlib>
#include "../Common/GUIDs.h"

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mf.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "crypt32.lib")

#ifndef MF_E_NO_MORE_ITEMS
#define MF_E_NO_MORE_ITEMS MAKE_HRESULT(SEVERITY_ERROR, 0x0811, 0x0001)
#endif
#define _CRT_SECURE_NO_WARNINGS

static void LogW(const wchar_t* fmt, ...)
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
}

static void GuidToW(REFGUID g, wchar_t* out, size_t cch)
{
    swprintf_s(out, cch, L"{%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
        g.Data1, g.Data2, g.Data3,
        g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3],
        g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
}

static bool Sha256Hex(const BYTE* data, DWORD len, wchar_t* out, size_t cch)
{
    HCRYPTPROV hProv = 0;
    if (!CryptAcquireContext(&hProv, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT)) return false;
    bool ok = false;
    HCRYPTKEY hHash = 0;
    if (CryptCreateHash(hProv, CALG_SHA_256, 0, 0, &hHash)) {
        if (CryptHashData(hHash, data, len, 0)) {
            BYTE digest[32] = { 0 };
            DWORD dl = sizeof(digest);
            if (CryptGetHashParam(hHash, HP_HASHVAL, digest, &dl, 0) && dl == sizeof(digest)) {
                char hex[65] = { 0 };
                for (int i = 0; i < 32; ++i)
                    sprintf_s(&hex[i * 2], 3, "%02x", digest[i]);
                swprintf_s(out, cch, L"%hs", hex);
                ok = true;
            }
        }
        CryptDestroyKey(hHash);
    }
    CryptReleaseContext(hProv, 0);
    return ok;
}

static void SaveBMP(const wchar_t* filename, const BYTE* rgb32Data, int width, int height, int stride)
{
    // Convert RGB32 (BGRX in memory) to 24-bit BGR for BMP
    int rowBytes = width * 3;
    // Pad to 4-byte boundary
    int paddedRow = (rowBytes + 3) & ~3;

    // BMP header
    FILE* f = _wfopen(filename, L"wb");
    if (!f) {
        LogW(L"SaveBMP: cannot open %s (errno=%d)", filename, errno);
        return;
    }

    // File header
    DWORD fileSize = 14 + 40 + paddedRow * height;
    std::array<unsigned char, 14> fileHeader = {
        'B', 'M',
        (unsigned char)(fileSize & 0xFF), (unsigned char)((fileSize >> 8) & 0xFF),
        (unsigned char)((fileSize >> 16) & 0xFF), (unsigned char)((fileSize >> 24) & 0xFF),
        0, 0, 0, 0,
        54, 0, 0, 0
    };
    fwrite(fileHeader.data(), 1, fileHeader.size(), f);

    // DIB header
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

    // Pixel data (bottom-up)
    unsigned char* dstRow = (unsigned char*)malloc(paddedRow);
    for (int y = height - 1; y >= 0; --y) {
        const BYTE* srcRow = rgb32Data + (SIZE_T)y * stride;
        for (int x = 0; x < width; ++x) {
            dstRow[x * 3] = srcRow[x * 4];     // B (buffer is BGRA, B at +0)
            dstRow[x * 3 + 1] = srcRow[x * 4 + 1]; // G
            dstRow[x * 3 + 2] = srcRow[x * 4 + 2]; // R
        }
        fwrite(dstRow, 1, paddedRow, f);
    }
    free(dstRow);

    fclose(f);
    LogW(L"Saved: %s", filename);
}

static int Clip255(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }

// BT.601 limited-range NV12 -> BGRX, then SaveBMP (inverse of the
// MediaSource ConvertRgb32ToNv12; verified round-trip against an RGB32 frame).
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

struct CToken : public IUnknown
{
    ULONG m_ref = 1;
    HRESULT QueryInterface(REFIID riid, void** ppv) override
    {
        if (riid == IID_IUnknown) { *ppv = static_cast<IUnknown*>(this); AddRef(); return S_OK; }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG AddRef() override { return InterlockedIncrement((LONG*)&m_ref); }
    ULONG Release() override { ULONG r = InterlockedDecrement((LONG*)&m_ref); if (r == 0) delete this; return r; }
};

// ---------------------------------------------------------------------------
// Direct mode: CoCreateInstance by CLSID (existing behavior)
// ---------------------------------------------------------------------------
static int RunDirectMode(int numFrames, const wchar_t* outputPrefix)
{
    LogW(L"--- direct mode (CLSID): frames=%d out='%s'", numFrames, outputPrefix);

    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr)) { LogW(L"CoInitializeEx failed: 0x%08X", hr); return 1; }

    hr = MFStartup(MF_VERSION);
    if (FAILED(hr)) { LogW(L"MFStartup failed: 0x%08X", hr); CoUninitialize(); return 1; }

    // Create the media source
    ATL::CComPtr<IMFMediaSource> pSource;
    hr = CoCreateInstance(CLSID_VCamMediaSource, nullptr, CLSCTX_INPROC_SERVER, IID_IMFMediaSource, (void**)&pSource);
    if (FAILED(hr)) {
        LogW(L"CoCreateInstance failed: 0x%08X", hr);
        MFShutdown();
        CoUninitialize();
        return 1;
    }

    // Create presentation descriptor
    ATL::CComPtr<IMFPresentationDescriptor> pPD;
    hr = pSource->CreatePresentationDescriptor(&pPD);
    if (FAILED(hr)) {
        LogW(L"CreatePresentationDescriptor failed: 0x%08X", hr);
        pPD = nullptr;
        pSource = nullptr;
        MFShutdown();
        CoUninitialize();
        return 1;
    }

    // Negotiate NV12 640x480 via IMFMediaSource2::SetMediaType before Start.
    {
        ATL::CComPtr<IMFMediaSource2> pSrc2;
        HRESULT hrQI = pSource->QueryInterface(IID_PPV_ARGS(&pSrc2));
        {
            wchar_t g[64] = L"?";
            GuidToW(__uuidof(IMFMediaSource2), g, _countof(g));
            LogW(L"direct QI src=%p iid=%s -> hr=0x%08X pSrc2=%p", pSource.p, g, (unsigned)hrQI, pSrc2.p);
        }
        ATL::CComPtr<IMFMediaType> pType;
        HRESULT hrMt = MFCreateMediaType(&pType);
        if (SUCCEEDED(hrQI) && pSrc2 && SUCCEEDED(hrMt) && pType) {
            pType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
            pType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
            MFSetAttributeSize(pType, MF_MT_FRAME_SIZE, 640, 480);
            MFSetAttributeRatio(pType, MF_MT_FRAME_RATE, 30, 1);
            pType->SetUINT32(MF_MT_DEFAULT_STRIDE, 640);
            pType->SetUINT32(MF_MT_SAMPLE_SIZE, 640 * 480 * 3 / 2);
            pType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
            HRESULT hrSet = pSrc2->SetMediaType(0, pType);
            LogW(L"direct Src2.SetMediaType NV12 640x480 -> 0x%08X (QI=0x%08X)", (unsigned)hrSet, (unsigned)hrQI);
        } else {
            LogW(L"direct SetMediaType negotiate failed QI=0x%08X MT=0x%08X", (unsigned)hrQI, (unsigned)hrMt);
        }
        pType = nullptr;
        pSrc2 = nullptr;
    }

    // Start the source
    // Текущий тип стрима 0 (после Src2-переговоров выше) — авторитетный размер
    // BMP: натив v2 (не 720p) тоже понимается, хардкода 1280x720 больше нет.
    GUID dirSub = {};
    UINT32 dirW = 0, dirH = 0;
    bool dirKnown = false;
    {
        BOOL bSel = FALSE;
        ATL::CComPtr<IMFStreamDescriptor> pSD;
        if (SUCCEEDED(pPD->GetStreamDescriptorByIndex(0, &bSel, &pSD)) && pSD) {
            ATL::CComPtr<IMFMediaTypeHandler> pMTH;
            if (SUCCEEDED(pSD->GetMediaTypeHandler(&pMTH)) && pMTH) {
                ATL::CComPtr<IMFMediaType> pCur;
                if (SUCCEEDED(pMTH->GetCurrentMediaType(&pCur)) && pCur) {
                    GUID major = {}, sub = {};
                    UINT32 w = 0, h = 0;
                    if (SUCCEEDED(pCur->GetGUID(MF_MT_MAJOR_TYPE, &major)) &&
                        SUCCEEDED(pCur->GetGUID(MF_MT_SUBTYPE, &sub)) &&
                        SUCCEEDED(MFGetAttributeSize(pCur, MF_MT_FRAME_SIZE, &w, &h)) &&
                        major == MFMediaType_Video && w > 0 && h > 0) {
                        dirSub = sub; dirW = w; dirH = h; dirKnown = true;
                        LogW(L"direct stream0 currentType: size=%ux%u %s", w, h,
                             (sub == MFVideoFormat_NV12) ? L"NV12" : L"RGB32");
                    }
                }
            }
        }
    }
    PROPVARIANT vtStart;
    vtStart.vt = VT_EMPTY;    hr = pSource->Start(pPD, nullptr, &vtStart);
    if (FAILED(hr)) {
        LogW(L"Start failed: 0x%08X", hr);
        pPD = nullptr;
        pSource = nullptr;
        MFShutdown();
        CoUninitialize();
        return 1;
    }
    pPD = nullptr;

    LogW(L"Source started. Waiting for stream...");

    // Pump events to get the stream
    ATL::CComPtr<IMFMediaStream> pStream;
    for (int i = 0; i < 100; ++i) {
        ATL::CComPtr<IMFMediaEvent> pEvent;
        hr = pSource->GetEvent(MF_EVENT_FLAG_NO_WAIT, &pEvent);
        if (hr == MF_E_NO_MORE_ITEMS) break;
        if (FAILED(hr)) continue;

        MediaEventType met;
        pEvent->GetType(&met);
        if (met == MENewStream) {
            PROPVARIANT vt;
            vt.vt = VT_UNKNOWN;
            pEvent->GetValue(&vt);
            pStream = static_cast<IMFMediaStream*>(vt.punkVal); // AddRef в operator=
            PropVariantClear(&vt);
            LogW(L"Got stream.");
            break;
        }
        pEvent = nullptr;
    }

    if (pStream == nullptr) {
        LogW(L"Failed to get stream.");
        pSource->Shutdown();
        pSource = nullptr;
        MFShutdown();
        CoUninitialize();
        return 1;
    }

    // Request samples
    LogW(L"Requesting %d frames...", numFrames);
    int framesReceived = 0;
    for (int i = 0; i < numFrames * 3; ++i) { // Request extra to account for timing
        if (framesReceived >= numFrames) break;

        ATL::CComPtr<CToken> pToken;
        pToken.Attach(new CToken());

        hr = pStream->RequestSample(pToken);
        if (FAILED(hr)) {
            pToken = nullptr;
            break;
        }

        // Pump stream events with timeout
        ULONGLONG st0 = GetTickCount64();
        bool gotSample = false;
        while (!gotSample && (GetTickCount64() - st0) < 2000) {
            ATL::CComPtr<IMFMediaEvent> pEvent;
            hr = pStream->GetEvent(MF_EVENT_FLAG_NO_WAIT, &pEvent);
            if (FAILED(hr)) { Sleep(20); continue; }

            MediaEventType met;
            pEvent->GetType(&met);
            if (met == MEMediaSample) {
                PROPVARIANT vt;
                vt.vt = VT_UNKNOWN;
                pEvent->GetValue(&vt);
                ATL::CComPtr<IMFSample> pSample;
                pSample = static_cast<IMFSample*>(vt.punkVal); // AddRef в operator=
                PropVariantClear(&vt);

                // Get buffer
                ATL::CComPtr<IMFMediaBuffer> pBuffer;
                pSample->ConvertToContiguousBuffer(&pBuffer);
                if (pBuffer) {
                    BYTE* pBits = nullptr;
                    DWORD maxLen, curLen;
                    pBuffer->Lock(&pBits, &maxLen, &curLen);
                    if (pBits) {
                        wchar_t bmpName[MAX_PATH];
                        swprintf_s(bmpName, L"%s_%03d.bmp", outputPrefix, framesReceived);
                        LogW(L"direct frame %d: curLen=%u maxLen=%u", framesReceived, curLen, maxLen);
                        const bool rgb32 = dirKnown
                            && dirSub == MFVideoFormat_RGB32
                            && dirW > 0 && dirH > 0
                            && (UINT64)curLen >= (UINT64)dirW * dirH * 4;
                        const bool nv12 = dirKnown
                            && dirSub == MFVideoFormat_NV12
                            && dirW > 0 && dirH > 0
                            && (UINT64)curLen >= (UINT64)dirW * dirH * 3 / 2;
                        if (rgb32) {
                            SaveBMP(bmpName, pBits, (int)dirW, (int)dirH, (int)(dirW * 4));
                        } else if (nv12) {
                            SaveNv12AsBmp(bmpName, pBits, (int)dirW, (int)dirH);
                        } else if (!dirKnown && curLen == 640 * 480 * 3 / 2) {
                            SaveNv12AsBmp(bmpName, pBits, 640, 480);
                        } else if (!dirKnown) {
                            SaveBMP(bmpName, pBits, 1280, 720, 5120);
                        } else {
                            LogW(L"direct frame %d: buffer does not match negotiated %ux%u (%u bytes) — skipped",
                                 framesReceived, dirW, dirH, curLen);
                        }
                    }
                    pBuffer->Unlock();
                    pBuffer = nullptr;
                }

                pSample = nullptr;
                framesReceived++;
                LogW(L"Frame %d received.", framesReceived);
                pEvent = nullptr;
                gotSample = true;
                break;
            }
            pEvent = nullptr;
        }

        pToken = nullptr;
        Sleep(40); // ~30 fps
    }

    LogW(L"\nTotal frames received: %d", framesReceived);

    // Stop and cleanup
    pStream = nullptr;
    pSource->Shutdown();
    pSource = nullptr;
    MFShutdown();
    CoUninitialize();
    return framesReceived >= numFrames ? 0 : 1;
}

// ---------------------------------------------------------------------------
// Device mode: MFEnumDeviceSources -> activate "the camera" AS A DEVICE ->
// pump 10 frames, one BMP + SHA256 per frame.
// Usage: CaptureTest.exe device [strict] [nv12] [nameFilter] [width] [height] [outputPrefix]
// Exit: 0 = captured all frames, 1 = failure, 2 = no device matching filter.
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// Device mode helpers
// ---------------------------------------------------------------------------

// RAII-сессия MF/COM: уборка на любом выходе — блок MFShutdown +
// CoUninitialize больше не дублируется перед каждым ранним return.
struct MfSession {
    bool comOk = false;
    bool mfOk = false;
    ~MfSession()
    {
        if (mfOk) MFShutdown();
        if (comOk) CoUninitialize();
    }
};

// Текущий тип stream 0: из него считается формат буферов сэмплов
// (BMP пишем по нему, а не по захардкоженному 1280x720).
struct Stream0Info {
    GUID major{};
    GUID sub{};
    UINT32 w = 0;
    UINT32 h = 0;
    bool known = false;
};

// Печатает потоки PD, по запросу ставит нужный тип на stream 0 и запоминает
// текущий тип. pPD отдаём наружу — он нужен для Start().
static HRESULT InspectStreams(IMFMediaSource* pSource, UINT32 reqW, UINT32 reqH,
                              bool wantNv12, Stream0Info& s0,
                              ATL::CComPtr<IMFPresentationDescriptor>& pPD)
{
    HRESULT hr = S_OK;
    hr = pSource->CreatePresentationDescriptor(&pPD);
    LogW(L"CreatePresentationDescriptor: hr=0x%08X", hr);
    if (SUCCEEDED(hr)) {
        DWORD nStreams = 0;
        hr = pPD->GetStreamDescriptorCount(&nStreams);
        LogW(L"  GetStreamDescriptorCount: hr=0x%08X count=%u", hr, nStreams);
        for (DWORD s = 0; s < nStreams; ++s) {
            BOOL bSelected = FALSE;
            ATL::CComPtr<IMFStreamDescriptor> pSD;
            hr = pPD->GetStreamDescriptorByIndex(s, &bSelected, &pSD);
            if (FAILED(hr) || !pSD) {
                LogW(L"  GetStreamDescriptorByIndex(%u): hr=0x%08X", s, hr);
                continue;
            }
            wchar_t sname[256] = L"(none)";
            DWORD sid = 0;
            PROPVARIANT vt;
            PropVariantInit(&vt);
            if (SUCCEEDED(pSD->GetItem(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &vt))
                && vt.vt == VT_BSTR && vt.bstrVal) {
                wcsncpy_s(sname, _countof(sname), vt.bstrVal, 255);
            }
            PropVariantClear(&vt);
            pSD->GetStreamIdentifier(&sid);
            LogW(L"  stream[%u] name='%s' streamId=%u selected=%d", s, sname, sid, (int)bSelected);

            ATL::CComPtr<IMFMediaTypeHandler> pMTH;
            hr = pSD->GetMediaTypeHandler(&pMTH);
            if (SUCCEEDED(hr) && pMTH) {
                if ((s == 0 || sid == 0) && reqW > 0 && reqH > 0) {
                    DWORD countTypes = 0;
                    pMTH->GetMediaTypeCount(&countTypes);
                    for (DWORD t = 0; t < countTypes; ++t) {
                        ATL::CComPtr<IMFMediaType> pMTType;
                        if (SUCCEEDED(pMTH->GetMediaTypeByIndex(t, &pMTType)) && pMTType) {
                            UINT32 w = 0, h = 0;
                            MFGetAttributeSize(pMTType, MF_MT_FRAME_SIZE, &w, &h);
                            GUID sub = {};
                            pMTType->GetGUID(MF_MT_SUBTYPE, &sub);
                            GUID want = wantNv12 ? MFVideoFormat_NV12 : MFVideoFormat_RGB32;
                            if (w == reqW && h == reqH && sub == want) {
                                pMTH->SetCurrentMediaType(pMTType);
                                LogW(L"Set requested media type: %ux%u %s", w, h, wantNv12 ? L"NV12" : L"RGB32");
                                pMTType = nullptr;
                                break;
                            }
                            pMTType = nullptr;
                        }
                    }
                }

                // Capture the CURRENT media type of the stream we start (s==0 / sid==0):
                // this is the format the sample buffers will actually be.
                ATL::CComPtr<IMFMediaType> pCur;
                hr = pMTH->GetCurrentMediaType(&pCur);
                if (SUCCEEDED(hr) && pCur && (s == 0 || sid == 0) && !s0.known) {
                    GUID major{}, sub{};
                    UINT32 w = 0, h = 0;
                    pCur->GetGUID(MF_MT_MAJOR_TYPE, &major);
                    pCur->GetGUID(MF_MT_SUBTYPE, &sub);
                    MFGetAttributeSize(pCur, MF_MT_FRAME_SIZE, &w, &h);
                    if (major == MFMediaType_Video && w > 0 && h > 0) {
                        s0.major = major;
                        s0.sub = sub;
                        s0.w = w;
                        s0.h = h;
                        s0.known = true;
                        wchar_t mg[64], sg[64];
                        GuidToW(major, mg, _countof(mg));
                        GuidToW(sub, sg, _countof(sg));
                        LogW(L"    stream0 currentType: major=%s sub=%s size=%ux%u (stride=%u for RGB32)", mg, sg, w, h, w * 4);
                    }
                    pCur = nullptr;
                } else if (pCur) {
                    pCur = nullptr;
                }

                ATL::CComPtr<IMFMediaType> pMT;
                hr = pMTH->GetMediaTypeByIndex(0, &pMT);
                if (SUCCEEDED(hr) && pMT) {
                    GUID major{}, sub{};
                    UINT32 w = 0, h = 0, num = 0, den = 0;
                    pMT->GetGUID(MF_MT_MAJOR_TYPE, &major);
                    pMT->GetGUID(MF_MT_SUBTYPE, &sub);
                    MFGetAttributeSize(pMT, MF_MT_FRAME_SIZE, &w, &h);
                    MFGetAttributeRatio(pMT, MF_MT_FRAME_RATE, &num, &den);
                    wchar_t mg[64], sg[64];
                    GuidToW(major, mg, _countof(mg));
                    GuidToW(sub, sg, _countof(sg));
                    LogW(L"    mediaType[0]: major=%s sub=%s size=%ux%u rate=%u/%u", mg, sg, w, h, num, den);
                    pMT = nullptr;
                } else {
                    LogW(L"    GetMediaTypeByIndex(0): hr=0x%08X", hr);
                }
                pMTH = nullptr;
            } else {
                LogW(L"    GetMediaTypeHandler: hr=0x%08X", hr);
            }
            pSD = nullptr;
        }
        hr = pPD->SelectStream(0);
        LogW(L"SelectStream(0): hr=0x%08X", hr);
    }
    return hr;
}

// 6. Pump samples: 10 frames, 2 s per frame
// (this pruned SDK's IMFMediaStream has no GetMediaType; the sample buffer
//  length is the evidence: 1280*720*4 = 3686400 bytes = RGB32 1280x720)
// numFrames кадров, 2 с на кадр; каждый — sha256 в лог и BMP/raw на диск.
// Возвращает число принятых кадров.
static int PumpFrames(IMFMediaStream* pStream, int numFrames,
                      const wchar_t* outputPrefix, const Stream0Info& s0)
{
    HRESULT hr = S_OK;
    int framesReceived = 0;
    ULONGLONG deadline = GetTickCount64() + 30000;
    while (framesReceived < numFrames && GetTickCount64() < deadline) {
        ATL::CComPtr<CToken> pToken;
        pToken.Attach(new CToken());
        hr = pStream->RequestSample(pToken);
        if (FAILED(hr)) {
            LogW(L"RequestSample(frame %d): hr=0x%08X", framesReceived, hr);
            pToken = nullptr;
            break;
        }

        bool got = false;
        ULONGLONG st0 = GetTickCount64();
        while (!got && (GetTickCount64() - st0) < 2000) {
            ATL::CComPtr<IMFMediaEvent> pEvent;
            hr = pStream->GetEvent(MF_EVENT_FLAG_NO_WAIT, &pEvent);
            if (FAILED(hr)) { Sleep(20); continue; }
            MediaEventType met;
            pEvent->GetType(&met);
            if (met == MEMediaSample) {
                PROPVARIANT vt;
                vt.vt = VT_UNKNOWN;
                pEvent->GetValue(&vt);
                ATL::CComPtr<IMFSample> pSample;
                pSample = static_cast<IMFSample*>(vt.punkVal); // AddRef в operator=
                PropVariantClear(&vt);

                ATL::CComPtr<IMFMediaBuffer> pBuffer;
                hr = pSample->ConvertToContiguousBuffer(&pBuffer);
                if (SUCCEEDED(hr) && pBuffer) {
                    BYTE* pBits = nullptr;
                    DWORD maxLen = 0, curLen = 0;
                    if (SUCCEEDED(pBuffer->Lock(&pBits, &maxLen, &curLen)) && pBits) {
                        wchar_t hex[65];
                        bool hashed = Sha256Hex(pBits, curLen, hex, _countof(hex));
                        LogW(L"frame %d: curLen=%u (%llu KB) sha256=%s",
                            framesReceived, curLen, (unsigned long long)(curLen / 1024), hashed ? hex : L"(hash failed)");
                        wchar_t fname[512];
                        // Save a BMP when the current type is RGB32 (size from the PD,
                        // not hardcoded): reference VCamSample emits 1280x960 RGB32.
                        bool rgb32 = s0.known
                            && s0.major == MFMediaType_Video
                            && s0.sub == MFVideoFormat_RGB32
                            && s0.w > 0 && s0.h > 0
                            && curLen >= s0.w * s0.h * 4;
                        bool nv12 = s0.known
                            && s0.major == MFMediaType_Video
                            && s0.sub == MFVideoFormat_NV12
                            && s0.w > 0 && s0.h > 0
                            && curLen >= s0.w * s0.h * 3 / 2;
                        if (rgb32) {
                            swprintf_s(fname, L"%s_%03d.bmp", outputPrefix, framesReceived);
                            SaveBMP(fname, pBits, (int)s0.w, (int)s0.h, (int)(s0.w * 4));
                        } else if (nv12) {
                            swprintf_s(fname, L"%s_%03d.bmp", outputPrefix, framesReceived);
                            SaveNv12AsBmp(fname, pBits, (int)s0.w, (int)s0.h);
                        } else {
                            swprintf_s(fname, L"%s_%03d.raw", outputPrefix, framesReceived);
                            FILE* f = _wfopen(fname, L"wb");
                            if (f) {
                                fwrite(pBits, 1, curLen, f);
                                fclose(f);
                                LogW(L"Saved raw: %s (%u bytes)", fname, curLen);
                            }
                        }
                        // zero-check: first 64 bytes all zero?
                        bool allZero64 = true;
                        for (DWORD k = 0; k < 64 && k < curLen; ++k) if (pBits[k] != 0) { allZero64 = false; break; }
                        if (allZero64) LogW(L"  WARNING: first 64 bytes are all zero");
                    }
                    pBuffer->Unlock();
                    pBuffer = nullptr;
                } else {
                    LogW(L"frame %d: buffer access failed hr=0x%08X", framesReceived, hr);
                }

                pSample = nullptr;
                framesReceived++;
                got = true;
            }
            pEvent = nullptr;
        }
        if (!got) LogW(L"no MEMediaSample for frame %d within 2 s", framesReceived);
        pToken = nullptr;
    }
    return framesReceived;
}

static int RunDeviceMode(int numFrames, const wchar_t* nameFilter, UINT32 reqW, UINT32 reqH, const wchar_t* outputPrefix, bool strict, bool wantNv12)
{
    LogW(L"--- device mode: filter='%s' strict=%d nv12=%d frames=%d res=%ux%u out='%s'", nameFilter, (int)strict, (int)wantNv12, numFrames, reqW, reqH, outputPrefix);

    MfSession session;
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr)) { LogW(L"CoInitializeEx failed: 0x%08X", hr); return 1; }
    session.comOk = true;

    hr = MFStartup(MF_VERSION);
    if (FAILED(hr)) { LogW(L"MFStartup failed: 0x%08X", hr); return 1; }
    session.mfOk = true;

    // 1. Enumerate video capture device sources
    IMFActivate** ppDevices = nullptr;
    UINT32 count = 0;
    ATL::CComPtr<IMFAttributes> pEnumAttr;
    hr = MFCreateAttributes(&pEnumAttr, 1);
    if (SUCCEEDED(hr)) {
        pEnumAttr->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
                           MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
        hr = MFEnumDeviceSources(pEnumAttr, &ppDevices, &count);
        pEnumAttr = nullptr; // до MFShutdown/CoUninitialize
    }
    LogW(L"MFEnumDeviceSources(VIDCAP filter): hr=0x%08X count=%u", hr, count);
    if (FAILED(hr)) {
        return 1;
    }

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

        wchar_t link[512] = L"(none)";
        PropVariantInit(&vt);
        if (SUCCEEDED(ppDevices[i]->GetItem(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID, &vt))
            && vt.vt == VT_BSTR && vt.bstrVal) {
            wcsncpy_s(link, _countof(link), vt.bstrVal, 511);
        }
        PropVariantClear(&vt);

        wchar_t typeGuid[64] = L"(none)";
        PropVariantInit(&vt);
        if (SUCCEEDED(ppDevices[i]->GetItem(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, &vt))
            && vt.vt == VT_CLSID && vt.puuid) {
            GuidToW(*vt.puuid, typeGuid, _countof(typeGuid));
        }
        PropVariantClear(&vt);

        LogW(L"  device[%u] name='%s' link='%s' sourceType=%s", i, name, link, typeGuid);
        if (strict) {
            // control-experiment mode: exact case-insensitive name match, no unnamed fallback
            if (match < 0 && hasName && _wcsicmp(name, nameFilter) == 0) match = (int)i;
        }
        else {
            if (match < 0 && hasName && wcsstr(name, nameFilter) != nullptr) match = (int)i;
            else if (firstUnnamed < 0 && !hasName) firstUnnamed = (int)i;
        }
    }

    // If the filter is all digits, treat it as an explicit device index (overrides name match).
    {
        bool allDigits = !strict && (nameFilter[0] != L'\0');
        for (const wchar_t* c = nameFilter; *c; ++c) if (*c < L'0' || *c > L'9') { allDigits = false; break; }
        if (allDigits) {
            int forced = _wtoi(nameFilter);
            if (forced >= 0 && forced < (int)count) {
                match = forced;
                LogW(L"Using forced device index %d (numeric filter)", match);
            } else {
                match = -1;
                LogW(L"Forced index %d out of range (count=%u)", forced, count);
            }
        }
    }

    if (match < 0 && firstUnnamed >= 0 && !strict) {
        match = firstUnnamed;
        LogW(L"No device matched filter '%s' by name; accepting first device without FRIENDLY_NAME (device[%d])", nameFilter, match);
    }

    if (match < 0) {
        LogW(L"NO DEVICE matching filter '%s' — exit 2", nameFilter);
        CoTaskMemFree(ppDevices);
        return 2;
    }
    LogW(L"Matched device[%d] to filter '%s'", match, nameFilter);

    ATL::CComPtr<IMFActivate> pAct;
    pAct.Attach(ppDevices[match]);
    pAct.p->AddRef(); // как раньше: владение из массива + AddRef (см. отчёт)
    CoTaskMemFree(ppDevices);

    // 2. Activate as IMFMediaSource
    ATL::CComPtr<IMFMediaSource> pSource;
    hr = pAct->ActivateObject(IID_IMFMediaSource, (void**)&pSource);
    LogW(L"ActivateObject(IID_IMFMediaSource): hr=0x%08X", hr);
    if (FAILED(hr)) {
        pAct = nullptr;
        return 1;
    }

    // 3. Presentation descriptor: log streams, select stream 0
    // Current media type of the stream we start (from the PD), used to size the
    // saved BMP correctly (reference VCamSample emits 1280x960 RGB32, not 1280x720).
    Stream0Info s0;

    ATL::CComPtr<IMFPresentationDescriptor> pPD;
    hr = InspectStreams(pSource, reqW, reqH, wantNv12, s0, pPD);
    if (FAILED(hr)) {
        pPD = nullptr;
        pSource = nullptr;
        pAct = nullptr;
        return 1;
    }


    // 4. Start (null stream ID, empty start position)
    PROPVARIANT vtStart;
    vtStart.vt = VT_EMPTY;
    hr = pSource->Start(pPD, nullptr, &vtStart);
    LogW(L"Start(pPD, nullptr, &wt): hr=0x%08X", hr);
    pPD = nullptr;
    if (FAILED(hr)) {
        pSource = nullptr;
        pAct = nullptr;
        return 1;
    }

    // 5. Wait for MENewStream (15 s deadline)
    ATL::CComPtr<IMFMediaStream> pStream;
    ULONGLONG t0 = GetTickCount64();
    while (!pStream && (GetTickCount64() - t0) < 15000) {
        ATL::CComPtr<IMFMediaEvent> pEvent;
        hr = pSource->GetEvent(MF_EVENT_FLAG_NO_WAIT, &pEvent);
        if (FAILED(hr)) { Sleep(20); continue; }
        MediaEventType met;
        pEvent->GetType(&met);
        LogW(L"  source event: type=%d (0x%08X)", (int)met, (unsigned)met);
        if (met == MENewStream) {
            PROPVARIANT vt;
            vt.vt = VT_UNKNOWN;
            pEvent->GetValue(&vt);
            pStream = static_cast<IMFMediaStream*>(vt.punkVal); // AddRef в operator=
            PropVariantClear(&vt);
            LogW(L"MENewStream received.");
        }
        pEvent = nullptr;
    }
    if (!pStream) {
        LogW(L"NO MENewStream within 15 s.");
        pSource->Stop();
        pSource->Shutdown();
        pSource = nullptr;
        pAct = nullptr;
        return 1;
    }

    // 5b. Wait for MEStreamStarted before the first RequestSample: the
    //     frameserver session proxy rejects RequestSample with
    //     MF_E_MEDIA_SOURCE_WRONGSTATE until source+stream have started.
    {
        bool started = false;
        ULONGLONG st0 = GetTickCount64();
        while (!started && (GetTickCount64() - st0) < 5000) {
            ATL::CComPtr<IMFMediaEvent> pEvent;
            HRESULT hrE = pStream->GetEvent(MF_EVENT_FLAG_NO_WAIT, &pEvent);
            if (SUCCEEDED(hrE)) {
                MediaEventType met;
                pEvent->GetType(&met);
                LogW(L"  stream event: type=%d (0x%08X)", (int)met, (unsigned)met);
                if (met == MEStreamStarted) started = true;
                pEvent = nullptr;
                continue;
            }
            hrE = pSource->GetEvent(MF_EVENT_FLAG_NO_WAIT, &pEvent);
            if (SUCCEEDED(hrE)) {
                MediaEventType met;
                pEvent->GetType(&met);
                LogW(L"  source event(2): type=%d (0x%08X)", (int)met, (unsigned)met);
                if (met == MEStreamStarted || met == MESourceStarted) started = true;
                pEvent = nullptr;
                continue;
            }
            Sleep(10);
        }
        LogW(started ? L"MEStreamStarted received."
                     : L"MEStreamStarted not received within 5 s (continuing).");
    }

    int framesReceived = PumpFrames(pStream, numFrames, outputPrefix, s0);

    LogW(L"total frames received: %d / %d", framesReceived, numFrames);

    // 7. Stop + clean shutdown
    hr = pSource->Stop();
    LogW(L"Stop: hr=0x%08X", hr);
    pStream = nullptr;
    pSource->Shutdown();
    pSource = nullptr;
    pAct = nullptr;
    return framesReceived >= numFrames ? 0 : 1;
}

// ---------------------------------------------------------------------------
// Inspect mode: enumerate ALL VIDCAP devices and dump every stream's full
// media-type list. No capture. Usage: CaptureTest.exe inspect
// ---------------------------------------------------------------------------
static int RunInspectMode()
{
    LogW(L"--- inspect mode: enumerate all VIDCAP devices, dump media types");

    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr)) { LogW(L"CoInitializeEx failed: 0x%08X", hr); return 1; }

    hr = MFStartup(MF_VERSION);
    if (FAILED(hr)) { LogW(L"MFStartup failed: 0x%08X", hr); CoUninitialize(); return 1; }

    IMFActivate** ppDevices = nullptr;
    UINT32 count = 0;
    ATL::CComPtr<IMFAttributes> pEnumAttr;
    hr = MFCreateAttributes(&pEnumAttr, 1);
    if (SUCCEEDED(hr)) {
        pEnumAttr->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
                           MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
        hr = MFEnumDeviceSources(pEnumAttr, &ppDevices, &count);
        pEnumAttr = nullptr; // до MFShutdown/CoUninitialize
    }
    LogW(L"MFEnumDeviceSources(VIDCAP): hr=0x%08X count=%u", hr, count);
    if (FAILED(hr) || count == 0) {
        if (ppDevices) CoTaskMemFree(ppDevices);
        MFShutdown();
        CoUninitialize();
        return count == 0 ? 2 : 1;
    }

    for (UINT32 i = 0; i < count; ++i) {
        wchar_t name[256] = L"(none)";
        PROPVARIANT vt;
        PropVariantInit(&vt);
        if (SUCCEEDED(ppDevices[i]->GetItem(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &vt))
            && vt.vt == VT_BSTR && vt.bstrVal) {
            wcsncpy_s(name, _countof(name), vt.bstrVal, 255);
        }
        PropVariantClear(&vt);
        LogW(L"  device[%u] name='%s'", i, name);

        ATL::CComPtr<IMFMediaSource> pSrc;
        hr = ppDevices[i]->ActivateObject(IID_IMFMediaSource, (void**)&pSrc);
        if (FAILED(hr)) {
            LogW(L"    ActivateObject(IID_IMFMediaSource): hr=0x%08X", hr);
            continue;
        }
        ATL::CComPtr<IMFPresentationDescriptor> pPD;
        hr = pSrc->CreatePresentationDescriptor(&pPD);
        if (SUCCEEDED(hr)) {
            DWORD ns = 0;
            pPD->GetStreamDescriptorCount(&ns);
            for (DWORD s = 0; s < ns; ++s) {
                BOOL bSel = FALSE;
                ATL::CComPtr<IMFStreamDescriptor> pSD;
                if (SUCCEEDED(pPD->GetStreamDescriptorByIndex(s, &bSel, &pSD))) {
                    ATL::CComPtr<IMFMediaTypeHandler> pMTH;
                    if (SUCCEEDED(pSD->GetMediaTypeHandler(&pMTH))) {
                        DWORD mtCount = 0;
                        pMTH->GetMediaTypeCount(&mtCount);
                        LogW(L"    stream[%u] selected=%d mediaTypes=%u", s, (int)bSel, mtCount);
                        for (DWORD m = 0; m < mtCount; ++m) {
                            ATL::CComPtr<IMFMediaType> pMT;
                            if (SUCCEEDED(pMTH->GetMediaTypeByIndex(m, &pMT))) {
                                GUID major{}, sub{};
                                UINT32 w = 0, h = 0, num = 0, den = 0;
                                pMT->GetGUID(MF_MT_MAJOR_TYPE, &major);
                                pMT->GetGUID(MF_MT_SUBTYPE, &sub);
                                MFGetAttributeSize(pMT, MF_MT_FRAME_SIZE, &w, &h);
                                MFGetAttributeRatio(pMT, MF_MT_FRAME_RATE, &num, &den);
                                wchar_t mg[64], sg[64];
                                GuidToW(major, mg, _countof(mg));
                                GuidToW(sub, sg, _countof(sg));
                                LogW(L"      type[%u] major=%s sub=%s size=%ux%u rate=%u/%u", m, mg, sg, w, h, num, den);
                                pMT = nullptr;
                            }
                        }
                        pMTH = nullptr;
                    }
                    pSD = nullptr;
                }
            }
            pPD = nullptr;
        } else {
            LogW(L"    CreatePresentationDescriptor: hr=0x%08X", hr);
        }
        pSrc->Shutdown();
        pSrc = nullptr;
    }

    CoTaskMemFree(ppDevices);
    MFShutdown();
    CoUninitialize();
    return 0;
}

int wmain(int argc, wchar_t* argv[])
{
    wprintf(L"VCam Capture Test - captures frames from the virtual camera\n");
    wprintf(L"Usage: CaptureTest.exe [numFrames] [outputPrefix]\n");
    wprintf(L"       CaptureTest.exe device [strict] [nv12] [nameFilter|index] [width] [height] [outputPrefix]\n");
    wprintf(L"       CaptureTest.exe inspect\n");
    if (argc >= 2 && _wcsicmp(argv[1], L"inspect") == 0) {
        int rc = RunInspectMode();
        wprintf(L"inspect exit code: %d\n", rc);
        return rc;
    }

    if (argc >= 2 && _wcsicmp(argv[1], L"device") == 0) {
        // 'strict' => exact case-insensitive name match, no unnamed-device fallback
        // 'nv12'   => negotiate NV12 instead of RGB32 and save via YUV->RGB
        bool strict = false;
        bool nv12 = false;
        int a = 2;
        while (argc > a) {
            if (_wcsicmp(argv[a], L"strict") == 0) { strict = true; ++a; }
            else if (_wcsicmp(argv[a], L"nv12") == 0) { nv12 = true; ++a; }
            else break;
        }
        const wchar_t* nameFilter = (argc > a) ? argv[a] : L"VCam";
        UINT32 reqW = (argc > a + 1) ? _wtoi(argv[a + 1]) : 0;
        UINT32 reqH = (argc > a + 2) ? _wtoi(argv[a + 2]) : 0;
        const wchar_t* outputPrefix = (argc > a + 3) ? argv[a + 3] : L"frame";
        int rc = RunDeviceMode(10, nameFilter, reqW, reqH, outputPrefix, strict, nv12);
        wprintf(L"device mode exit code: %d\n", rc);
        return rc;
    }

    int numFrames = 10;
    const wchar_t* outputPrefix = L"frame";
    if (argc >= 2) numFrames = _wtoi(argv[1]);
    if (argc >= 3) outputPrefix = argv[2];
    return RunDirectMode(numFrames, outputPrefix);
}
