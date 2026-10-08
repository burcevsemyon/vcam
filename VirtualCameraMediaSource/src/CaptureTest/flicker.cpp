#include "CaptureTestPCH.h"
#include "CaptureTestTypes.h"
#include "CaptureTestCommon.h"

struct FlickerCtx {
    std::vector<FrameRec> recs;
    double threshold = 40.0;
    const wchar_t* prefix = L"cap";
    bool saveBlack = false;
};

static void FlickerCallback(void* ctx, const FrameCtx& fc)
{
    FlickerCtx* c = (FlickerCtx*)ctx;
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

int RunFlickerMode(const ModeArgs& a)
{
    SetLogFile(a.logFile);
    FlickerCtx c;
    c.threshold = a.threshold;
    c.prefix = a.prefix;
    c.saveBlack = a.saveBlack;

    CapOptions opt;
    opt.nameFilter = a.nameFilter;
    opt.reqW = a.reqW;
    opt.reqH = a.reqH;
    opt.wantNv12 = a.nv12;
    opt.numFrames = a.frames;
    opt.intervalMs = a.intervalMs;
    opt.deadlineMs = (ULONGLONG)(a.frames * (std::max)(a.intervalMs, 1) + 30000);

    int rc = RunCaptureSession(opt, FlickerCallback, &c, nullptr, nullptr);

    int blackCount = 0;
    for (const auto& r : c.recs) if (r.black) ++blackCount;

    int transitions = 0, maxSeries = 0, seriesCount = 0, curSeries = 0;
    bool inBlack = false;
    for (const auto& r : c.recs) {
        if (r.black && !inBlack) { inBlack = true; curSeries = 1; }
        else if (r.black && inBlack) { ++curSeries; }
        else if (!r.black && inBlack) {
            inBlack = false;
            ++transitions;
            if (curSeries > maxSeries) maxSeries = curSeries;
            ++seriesCount;
            curSeries = 0;
        }
    }
    if (inBlack) {
        ++transitions;
        if (curSeries > maxSeries) maxSeries = curSeries;
        ++seriesCount;
    }
    double avgSeries = seriesCount > 0 ? (double)blackCount / seriesCount : 0;

    double meanAll = 0;
    for (const auto& r : c.recs) meanAll += r.px.mean;
    if (!c.recs.empty()) meanAll /= c.recs.size();

    const wchar_t* verdict = (blackCount <= a.allowBlack) ? L"OK" : L"FLICKER";

    LogW(L"--- flicker summary: frames=%d black=%d allowBlack=%d transitions=%d maxSeries=%d avgSeries=%.1f mean=%.1f verdict=%s",
         (int)c.recs.size(), blackCount, a.allowBlack, transitions, maxSeries, avgSeries, meanAll, verdict);

    if (a.saveBlack && blackCount > 0) {
        for (const auto& r : c.recs) {
            if (r.black) {
                wchar_t path[512];
                swprintf_s(path, L"%s_black_%d.bmp", a.prefix, r.index);
                int w = 1280, h = 720;
                SaveBMP(path, nullptr, w, h, w * 4);
                break;
            }
        }
    }

    if (a.json) {
        wprintf(L"{\"mode\":\"flicker\",\"frames\":%d,\"black\":%d,\"allowBlack\":%d,\"transitions\":%d,\"maxSeries\":%d,\"avgSeries\":%.1f,\"mean\":%.1f,\"threshold\":%.1f,\"verdict\":\"%s\"}\n",
                (int)c.recs.size(), blackCount, a.allowBlack, transitions, maxSeries, avgSeries, meanAll, a.threshold, verdict);
    }

    if (rc == 0 && blackCount <= a.allowBlack) return 0;
    return rc != 0 ? rc : 1;
}