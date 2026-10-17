#include "CaptureTestPCH.h"
#include "CaptureTestTypes.h"
#include "CaptureTestCommon.h"

static void NoopCallback(void*, const FrameCtx&) {}

int RunPerfMode(const ModeArgs& a)
{
    SetLogFile(a.logFile);
    std::vector<ULONGLONG> requestMs, gaps;
    CapOptions opt;
    opt.nameFilter = a.nameFilter;
    opt.reqW = a.reqW;
    opt.reqH = a.reqH;
    opt.wantNv12 = a.nv12;
    opt.numFrames = a.frames;
    opt.intervalMs = 0;
    opt.deadlineMs = (ULONGLONG)(a.frames * 200 + 30000);

    int rc = RunCaptureSession(opt, NoopCallback, nullptr, &requestMs, &gaps);

    double p50 = Percentile(requestMs, 0.50);
    double p90 = Percentile(requestMs, 0.90);
    double p99 = Percentile(requestMs, 0.99);
    double reqMax = requestMs.empty() ? 0 : (double)*std::max_element(requestMs.begin(), requestMs.end());
    double gapMean = 0, gapMax = 0;
    for (ULONGLONG g : gaps) { gapMean += (double)g; if ((double)g > gapMax) gapMax = (double)g; }
    if (!gaps.empty()) gapMean /= gaps.size();
    double fps = gapMean > 0 ? 1000.0 / gapMean : 0;
    const wchar_t* verdict = (p99 < 100 && rc == 0) ? L"OK" : L"SLOW";

    LogW(L"--- perf summary: frames=%d reqP50=%.0fms reqP90=%.0fms reqP99=%.0fms reqMax=%.0fms gapMean=%.0fms gapMax=%.0fms fps=%.1f verdict=%s",
         (int)requestMs.size(), p50, p90, p99, reqMax, gapMean, gapMax, fps, verdict);

    if (a.json) {
        wprintf(L"{\"mode\":\"perf\",\"frames\":%d,\"reqP50ms\":%.0f,\"reqP90ms\":%.0f,\"reqP99ms\":%.0f,\"reqMaxMs\":%.0f,\"gapMeanMs\":%.0f,\"gapMaxMs\":%.0f,\"fps\":%.1f,\"verdict\":\"%s\"}\n",
                (int)requestMs.size(), p50, p90, p99, reqMax, gapMean, gapMax, fps, verdict);
    }

    if (rc == 0 && p99 < 100) return 0;
    return rc != 0 ? rc : 1;
}