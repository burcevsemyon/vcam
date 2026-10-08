#include "CaptureTestPCH.h"
#include "CaptureTestTypes.h"
#include "CaptureTestCommon.h"

struct StressCtx {
    std::vector<FrameRec> recs;
    double threshold = 40.0;
};

static void StressCallback(void* ctx, const FrameCtx& fc)
{
    StressCtx* c = (StressCtx*)ctx;
    FrameRec r;
    r.index = fc.index;
    r.tMs = fc.tMs;
    r.requestMs = fc.requestMs;
    r.curLen = fc.curLen;
    int w = (int)fc.s0.dwWidth, h = (int)fc.s0.dwHeight;
    int stride = (fc.s0.sub == MFVideoFormat_NV12) ? w : (int)(fc.s0.dwWidth * 4);
    r.px = ComputePixelStats(fc.pBits, fc.curLen, w, h, stride, c->threshold);
    Sha256Hex(fc.pBits, fc.curLen, r.hash, _countof(r.hash));
    r.black = r.px.mean < c->threshold;
    c->recs.push_back(r);
}

int RunStressMode(const ModeArgs& a)
{
    SetLogFile(a.logFile);
    MfSession session;
    ATL::CComPtr<IMFActivate> pAct;
    ATL::CComPtr<IMFMediaSource> pSource;
    ATL::CComPtr<IMFMediaStream> pStream;
    Stream0Info s0;
    int rc = OpenDeviceSession(a.nameFilter, a.reqW, a.reqH, a.nv12,
                               session, pAct, pSource, pStream, s0);
    if (rc != 0) return rc;

    StressCtx c;
    c.threshold = a.threshold;
    ULONGLONG deadline = GetTickCount64() + (ULONGLONG)a.durationSec * 1000;
    ULONGLONG nextReport = GetTickCount64() + 10000;
    ULONGLONG wsPeak = 0;
    int framesReceived = 0;
    ULONGLONG prevT = 0;
    std::vector<ULONGLONG> gaps;

    while (GetTickCount64() < deadline) {
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
                        fc.index = framesReceived;
                        fc.curLen = curLen;
                        fc.requestMs = GetTickCount64() - st0;
                        fc.tMs = GetTickCount64();
                        fc.pBits = pBits;
                        fc.s0 = s0;
                        StressCallback(&c, fc);
                        pBuffer->Unlock();
                        ++framesReceived;
                        if (prevT) gaps.push_back(fc.tMs - prevT);
                        prevT = fc.tMs;
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
        if (a.intervalMs > 0) Sleep(a.intervalMs);

        if (GetTickCount64() >= nextReport) {
            PROCESS_MEMORY_COUNTERS_EX pmc;
            if (GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pmc, sizeof(pmc))
                && pmc.WorkingSetSize > wsPeak) wsPeak = pmc.WorkingSetSize;
            int black = 0;
            for (const auto& r : c.recs) if (r.black) ++black;
            LogW(L"stress progress: t=%dss frames=%d black=%d ws=%.1fMB",
                 (int)((GetTickCount64() - (deadline - (ULONGLONG)a.durationSec * 1000)) / 1000),
                 framesReceived, black, (double)pmc.WorkingSetSize / (1024 * 1024));
            nextReport += 10000;
        }
    }

    CloseDeviceSession(pStream, pSource, pAct);

    int blackCount = 0;
    for (const auto& r : c.recs) if (r.black) ++blackCount;
    double gapMean = 0;
    for (ULONGLONG g : gaps) gapMean += (double)g;
    if (!gaps.empty()) gapMean /= gaps.size();
    double fps = gapMean > 0 ? 1000.0 / gapMean : 0;
    const wchar_t* verdict = (blackCount == 0) ? L"OK" : L"DEGRADED";
    LogW(L"--- stress summary: duration=%ds frames=%d black=%d fps=%.1f wsPeak=%.1fMB verdict=%s",
         a.durationSec, framesReceived, blackCount, fps, (double)wsPeak / (1024 * 1024), verdict);

    if (a.json) {
        wprintf(L"{\"mode\":\"stress\",\"durationSec\":%d,\"frames\":%d,\"black\":%d,"
                L"\"fps\":%.1f,\"wsPeakMB\":%.1f,\"verdict\":\"%s\"}\n",
                a.durationSec, framesReceived, blackCount, fps,
                (double)wsPeak / (1024 * 1024), verdict);
    }
    if (rc == 0 && blackCount == 0) return 0;
    return rc != 0 ? rc : 1;
}