#include "CaptureTestPCH.h"
#include "CaptureTestTypes.h"
#include "CaptureTestCommon.h"

struct AnalyzeCtx {
    std::vector<FrameRec> recs;
    double threshold = 40.0;
};

static void AnalyzeCallback(void* ctx, const FrameCtx& fc)
{
    AnalyzeCtx* c = (AnalyzeCtx*)ctx;
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

int RunAnalyzeMode(const ModeArgs& a)
{
    SetLogFile(a.logFile);
    AnalyzeCtx c;
    c.threshold = a.threshold;

    CapOptions opt;
    opt.nameFilter = a.nameFilter;
    opt.reqW = a.reqW;
    opt.reqH = a.reqH;
    opt.wantNv12 = a.nv12;
    opt.numFrames = a.frames;
    opt.intervalMs = a.intervalMs;
    opt.deadlineMs = (ULONGLONG)(a.frames * (std::max)(a.intervalMs, 1) + 30000);

    int rc = RunCaptureSession(opt, AnalyzeCallback, &c, nullptr, nullptr);

    if (c.recs.empty()) {
        LogW(L"analyze: no frames");
        return rc != 0 ? rc : 1;
    }

    double meanAll = 0, stddevAll = 0;
    for (const auto& r : c.recs) meanAll += r.px.mean;
    meanAll /= c.recs.size();
    for (const auto& r : c.recs) stddevAll += (r.px.mean - meanAll) * (r.px.mean - meanAll);
    stddevAll = c.recs.size() > 1 ? sqrt(stddevAll / c.recs.size()) : 0;
    double contrast = meanAll > 0 ? stddevAll / meanAll : 0;

    const auto& last = c.recs.back();
    int w = (int)last.s0.dwWidth, h = (int)last.s0.dwHeight;
    int bands[10] = {};
    int bandH = h / 10;
    for (int b = 0; b < 10; ++b) {
        double sum = 0;
        int count = 0;
        int y0 = b * bandH;
        int y1 = (b == 9) ? h : (b + 1) * bandH;
        for (int y = y0; y < y1; y += 4) {
            const BYTE* row = last.pBits + (SIZE_T)y * w * 4;
            for (int x = 0; x < w; x += 4) {
                int lum = LuminanceBt601(row[x * 4 + 2], row[x * 4 + 1], row[x * 4]);
                sum += lum;
                ++count;
            }
        }
        bands[b] = count > 0 ? (int)(sum / count) : 0;
    }
    double topAvg = (bands[0] + bands[1]) / 2.0;
    double bottomAvg = (bands[8] + bands[9]) / 2.0;
    double midAvg = (bands[4] + bands[5]) / 2.0;
    bool letterbox = (topAvg < a.threshold && bottomAvg < a.threshold && midAvg >= a.threshold);
    bool topOnly = (topAvg >= a.threshold && bottomAvg < a.threshold);
    bool bottomOnly = (bottomAvg >= a.threshold && topAvg < a.threshold);

    const wchar_t* verdict = (!letterbox && !topOnly && !bottomOnly) ? L"OK" : L"ARTIFACT";

    LogW(L"--- analyze summary: frames=%d mean=%.1f stddev=%.1f contrast=%.3f letterbox=%d topOnly=%d bottomOnly=%d verdict=%s",
         (int)c.recs.size(), meanAll, stddevAll, contrast, (int)letterbox, (int)topOnly, (int)bottomOnly, verdict);

    if (a.json) {
        wprintf(L"{\"mode\":\"analyze\",\"frames\":%d,\"mean\":%.1f,\"stddev\":%.1f,\"contrast\":%.3f,\"topAvg\":%.1f,\"bottomAvg\":%.1f,\"midAvg\":%.1f,\"letterbox\":%s,\"topOnly\":%s,\"bottomOnly\":%s,\"verdict\":\"%s\"}\n",
                (int)c.recs.size(), meanAll, stddevAll, contrast, topAvg, bottomAvg, midAvg,
                letterbox ? L"true" : L"false", topOnly ? L"true" : L"false",
                bottomOnly ? L"true" : L"false", verdict);
    }

    if (rc == 0 && verdict == L"OK") return 0;
    return rc != 0 ? rc : 1;
}