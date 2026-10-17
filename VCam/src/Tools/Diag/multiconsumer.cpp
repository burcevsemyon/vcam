#include "CaptureTestPCH.h"
#include "CaptureTestTypes.h"
#include "CaptureTestCommon.h"

int RunMulticonsumerMode(const ModeArgs& a)
{
    MfSession session;
    ATL::CComPtr<IMFActivate> pAct;
    ATL::CComPtr<IMFMediaSource> pSource;
    ATL::CComPtr<IMFMediaStream> pStream;
    Stream0Info s0;
    int rc = OpenDeviceSession(a.nameFilter, a.reqW, a.reqH, a.nv12,
                               session, pAct, pSource, pStream, s0);
    if (rc != 0) return rc;

    bool firstGot = false;
    {
        ATL::CComPtr<CToken> pToken;
        pToken.Attach(new CToken());
        if (SUCCEEDED(pStream->RequestSample(pToken))) {
            ULONGLONG st0 = GetTickCount64();
            while (!firstGot && (GetTickCount64() - st0) < 3000) {
                ATL::CComPtr<IMFMediaEvent> pEvent;
                HRESULT hr = pStream->GetEvent(MF_EVENT_FLAG_NO_WAIT, &pEvent);
                if (FAILED(hr)) { Sleep(20); continue; }
                MediaEventType met;
                pEvent->GetType(&met);
                if (met == MEMediaSample) {
                    firstGot = true;
                    LogW(L"multiconsumer: first consumer received a sample");
                }
                pEvent = nullptr;
            }
        }
        pToken = nullptr;
    }
    if (!firstGot) {
        LogW(L"multiconsumer: first consumer got NO sample within 3 s — cannot test");
        CloseDeviceSession(pStream, pSource, pAct);
        return 1;
    }

    bool secondGot = false;
    {
        ATL::CComPtr<IMFMediaSource> pSource2;
        HRESULT hr = pAct->ActivateObject(IID_IMFMediaSource, (void**)&pSource2);
        LogW(L"multiconsumer: second ActivateObject hr=0x%08X", hr);
        if (SUCCEEDED(hr)) {
            ATL::CComPtr<IMFPresentationDescriptor> pPD2;
            hr = pSource2->CreatePresentationDescriptor(&pPD2);
            if (SUCCEEDED(hr)) {
                pPD2->SelectStream(0);
                PROPVARIANT vtStart;
                vtStart.vt = VT_EMPTY;
                HRESULT secondStartHr = pSource2->Start(pPD2, nullptr, &vtStart);
                LogW(L"multiconsumer: second Start hr=0x%08X", (unsigned)secondStartHr);
                if (SUCCEEDED(secondStartHr)) {
                    ATL::CComPtr<IMFMediaStream> pStream2;
                    ULONGLONG t0 = GetTickCount64();
                    while (!pStream2 && (GetTickCount64() - t0) < 5000) {
                        ATL::CComPtr<IMFMediaEvent> pEvent;
                        hr = pSource2->GetEvent(MF_EVENT_FLAG_NO_WAIT, &pEvent);
                        if (FAILED(hr)) { Sleep(20); continue; }
                        MediaEventType met;
                        pEvent->GetType(&met);
                        if (met == MENewStream) {
                            PROPVARIANT vt;
                            vt.vt = VT_UNKNOWN;
                            pEvent->GetValue(&vt);
                            pStream2 = static_cast<IMFMediaStream*>(vt.punkVal);
                            PropVariantClear(&vt);
                        }
                        pEvent = nullptr;
                    }
                    if (pStream2) {
                        ATL::CComPtr<CToken> pToken;
                        pToken.Attach(new CToken());
                        HRESULT secondRequestHr = pStream2->RequestSample(pToken);
                        LogW(L"multiconsumer: second RequestSample hr=0x%08X", (unsigned)secondRequestHr);
                        ULONGLONG st0 = GetTickCount64();
                        while (!secondGot && (GetTickCount64() - st0) < 3000) {
                            ATL::CComPtr<IMFMediaEvent> pEvent;
                            hr = pStream2->GetEvent(MF_EVENT_FLAG_NO_WAIT, &pEvent);
                            if (FAILED(hr)) { Sleep(20); continue; }
                            MediaEventType met;
                            pEvent->GetType(&met);
                            if (met == MEMediaSample) {
                                secondGot = true;
                                LogW(L"multiconsumer: SECOND consumer received a sample — monopoly NOT enforced");
                            }
                            pEvent = nullptr;
                        }
                        pToken = nullptr;
                    }
                }
                pPD2 = nullptr;
            }
            pSource2->Shutdown();
            pSource2 = nullptr;
        }
    }

    bool firstStillWorks = false;
    {
        ATL::CComPtr<CToken> pToken;
        pToken.Attach(new CToken());
        if (SUCCEEDED(pStream->RequestSample(pToken))) {
            ULONGLONG st0 = GetTickCount64();
            while (!firstStillWorks && (GetTickCount64() - st0) < 3000) {
                ATL::CComPtr<IMFMediaEvent> pEvent;
                HRESULT hr = pStream->GetEvent(MF_EVENT_FLAG_NO_WAIT, &pEvent);
                if (FAILED(hr)) { Sleep(20); continue; }
                MediaEventType met;
                pEvent->GetType(&met);
                if (met == MEMediaSample) firstStillWorks = true;
                pEvent = nullptr;
            }
        }
        pToken = nullptr;
    }

    CloseDeviceSession(pStream, pSource, pAct);

    const wchar_t* verdict = (!secondGot && firstStillWorks) ? L"OK" : L"MONOPOLY_BROKEN";
    LogW(L"--- multiconsumer summary: first=%d secondSamples=%d firstStillWorks=%d verdict=%s",
         (int)firstGot, (int)secondGot, (int)firstStillWorks, verdict);

    if (a.json) {
        wprintf(L"{\"mode\":\"multiconsumer\",\"firstSamples\":%d,\"secondSamples\":%d,"
                L"\"firstStillWorks\":%s,\"verdict\":\"%s\"}\n",
                (int)firstGot, (int)secondGot,
                firstStillWorks ? L"true" : L"false", verdict);
    }
    return (verdict == L"OK") ? 0 : 1;
}