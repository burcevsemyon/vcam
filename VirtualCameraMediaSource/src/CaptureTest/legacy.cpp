// Legacy-режимы CaptureTest, восстановленные из оригинального main.cpp
// (прямой захват по CLSID, device, inspect) — используются e2e_test.ps1.
#include "CaptureTestPCH.h"
#include "CaptureTestTypes.h"
#include "CaptureTestCommon.h"
#include "../Common/GUIDs.h"
#include <cerrno>

// --- локальные хелперы (внутренняя линковка, не мешают общим) ---------------

static void GuidToW(REFGUID g, wchar_t* out, size_t cch)
{
    swprintf_s(out, cch, L"{%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
        g.Data1, g.Data2, g.Data3,
        g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3],
        g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
}

static int Clip255(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }

// BT.601 limited-range NV12 -> BGRX, затем SaveBMP (обратная операция к
// ConvertRgb32ToNv12 в MediaSource; round-trip проверен на RGB32-кадре).
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

// Текущий тип stream 0 (legacy-поля: в общих типах другая структура).
struct LegS0 {
    GUID major{};
    GUID sub{};
    UINT32 w = 0;
    UINT32 h = 0;
    bool known = false;
};

// ---------------------------------------------------------------------------
// Direct mode: CoCreateInstance по CLSID (поведение оригинального main.cpp)
// Exit: 0 = все кадры получены, 1 = сбой.
// ---------------------------------------------------------------------------
int RunDirectMode(int numFrames, const wchar_t* outputPrefix)
{
    LogW(L"--- direct mode (CLSID): frames=%d out='%s'", numFrames, outputPrefix);

    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr)) { LogW(L"CoInitializeEx failed: 0x%08X", hr); return 1; }

    hr = MFStartup(MF_VERSION);
    if (FAILED(hr)) { LogW(L"MFStartup failed: 0x%08X", hr); CoUninitialize(); return 1; }

    ATL::CComPtr<IMFMediaSource> pSource;
    hr = pSource.CoCreateInstance(CLSID_VCamMediaSource, nullptr, CLSCTX_INPROC_SERVER);
    if (FAILED(hr)) {
        LogW(L"CoCreateInstance failed: 0x%08X", hr);
        MFShutdown();
        CoUninitialize();
        return 1;
    }

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

    // Переговоры NV12 640x480 через IMFMediaSource2::SetMediaType до Start.
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
    vtStart.vt = VT_EMPTY;
    hr = pSource->Start(pPD, nullptr, &vtStart);
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

    LogW(L"Requesting %d frames...", numFrames);
    int framesReceived = 0;
    for (int i = 0; i < numFrames * 3; ++i) { // лишние запросы на случай тайминга
        if (framesReceived >= numFrames) break;

        ATL::CComPtr<CToken> pToken;
        pToken.Attach(new CToken());

        hr = pStream->RequestSample(pToken);
        if (FAILED(hr)) {
            pToken = nullptr;
            break;
        }

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

    pStream = nullptr;
    pSource->Shutdown();
    pSource = nullptr;
    MFShutdown();
    CoUninitialize();
    return framesReceived >= numFrames ? 0 : 1;
}

// ---------------------------------------------------------------------------
// InspectStreams: печатает потоки PD, по запросу ставит нужный тип на stream 0
// и запоминает текущий тип. pPD отдаём наружу — он нужен для Start().
// ---------------------------------------------------------------------------
static HRESULT InspectStreams(IMFMediaSource* pSource, UINT32 reqW, UINT32 reqH,
                              bool wantNv12, LegS0& s0,
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

                // Текущий тип stream 0 — именно в таком формате придут сэмплы.
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

// 6. Прокачка сэмплов: numFrames кадров, 2 с на кадр
// (у этого SDK нет GetMediaType у IMFMediaStream; длина буфера сэмпла —
//  свидетельство формата: 1280*720*4 = 3686400 байт = RGB32 1280x720).
// sha256 каждого кадра пишется в лог; BMP/raw — на диск.
static int PumpFrames(IMFMediaStream* pStream, int numFrames,
                      const wchar_t* outputPrefix, const LegS0& s0)
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
                        // BMP — только если текущий тип RGB32/NV12 и буфер влезает.
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
                        // zero-check: первые 64 байта все нулевые?
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

// ---------------------------------------------------------------------------
// Device mode: MFEnumDeviceSources -> активируем «камеру» КАК УСТРОЙСТВО ->
// прокачиваем кадры, BMP + SHA256 на кадр.
// Usage: CaptureTest.exe device [strict] [nv12] [nameFilter|index] [width] [height] [outputPrefix]
// Exit: 0 = все кадры, 1 = сбой, 2 = нет устройства по фильтру.
// ---------------------------------------------------------------------------
int RunDeviceMode(int numFrames, const wchar_t* nameFilter, UINT32 reqW, UINT32 reqH,
                  const wchar_t* outputPrefix, bool strict, bool wantNv12)
{
    LogW(L"--- device mode: filter='%s' strict=%d nv12=%d frames=%d res=%ux%u out='%s'", nameFilter, (int)strict, (int)wantNv12, numFrames, reqW, reqH, outputPrefix);

    MfSession session;
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr)) { LogW(L"CoInitializeEx failed: 0x%08X", hr); return 1; }
    session.comOk = true;

    hr = MFStartup(MF_VERSION);
    if (FAILED(hr)) { LogW(L"MFStartup failed: 0x%08X", hr); return 1; }
    session.mfOk = true;

    // 1. Перечисление источников видеозахвата
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
            // control-experiment: точное совпадение имени без unnamed-fallback
            if (match < 0 && hasName && _wcsicmp(name, nameFilter) == 0) match = (int)i;
        }
        else {
            if (match < 0 && hasName && wcsstr(name, nameFilter) != nullptr) match = (int)i;
            else if (firstUnnamed < 0 && !hasName) firstUnnamed = (int)i;
        }
    }

    // Если фильтр состоит только из цифр — это явный индекс устройства
    // (перебивает совпадение по имени).
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
    pAct.p->AddRef(); // как раньше: вложение из массива + AddRef (см. отчет)
    CoTaskMemFree(ppDevices);

    // 2. Активация как IMFMediaSource
    ATL::CComPtr<IMFMediaSource> pSource;
    hr = pAct->ActivateObject(IID_IMFMediaSource, (void**)&pSource);
    LogW(L"ActivateObject(IID_IMFMediaSource): hr=0x%08X", hr);
    if (FAILED(hr)) {
        pAct = nullptr;
        return 1;
    }

    // 3. Presentation descriptor: лог потоков, выбор stream 0
    LegS0 s0;

    ATL::CComPtr<IMFPresentationDescriptor> pPD;
    hr = InspectStreams(pSource, reqW, reqH, wantNv12, s0, pPD);
    if (FAILED(hr)) {
        pPD = nullptr;
        pSource = nullptr;
        pAct = nullptr;
        return 1;
    }

    // 4. Start (null stream ID, пустая стартовая позиция)
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

    // 5. Ожидание MENewStream (дедлайн 15 с)
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

    // 5b. Ждём MEStreamStarted до первого RequestSample: прокси
    //     frameserver-сессии отвечает на RequestSample
    //     MF_E_MEDIA_SOURCE_WRONGSTATE, пока source+stream не стартовали.
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

    // 7. Stop + чистый выход
    hr = pSource->Stop();
    LogW(L"Stop: hr=0x%08X", hr);
    pStream = nullptr;
    pSource->Shutdown();
    pSource = nullptr;
    pAct = nullptr;
    return framesReceived >= numFrames ? 0 : 1;
}

// ---------------------------------------------------------------------------
// Inspect mode: перечисляет ВСЕ VIDCAP-устройства и печатает полный список
// медиатипов каждого потока. Без захвата. Usage: CaptureTest.exe inspect
// Exit: 0 = ок, 1 = сбой COM/MF, 2 = устройств нет.
// ---------------------------------------------------------------------------
int RunInspectMode()
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
