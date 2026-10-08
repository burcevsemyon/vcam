#include "CaptureTestPCH.h"
#include "CaptureTestTypes.h"
#include "CaptureTestCommon.h"

struct HotswitchCtx {
    std::vector<FrameRec> recs;
    double threshold = 40.0;
    const wchar_t* settingsFile = nullptr;
    ULONGLONG mtimeStart = 0;
    int switchFrame = -1;
};

static void HotswitchCallback(void* ctx, const FrameCtx& fc)
{
    HotswitchCtx* c = (HotswitchCtx*)ctx;
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
    if (c->settingsFile && c->switchFrame < 0) {
        ULONGLONG mt = FileMtimeMs(c->settingsFile);
        if (mt != 0 && mt != c->mtimeStart) {
            c->switchFrame = fc.index;
            LogW(L"hotswitch: settings.json changed at frame %d", fc.index);
        }
    }
    c->recs.push_back(r);
}

int RunHotswitchMode(const ModeArgs& a)
{
    if (!a.settingsFile) {
        LogW(L"hotswitch: --settings <path> required");
        return 2;
    }
    SetLogFile(a.logFile);
    HotswitchCtx c;
    c.threshold = a.threshold;
    c.settingsFile = a.settingsFile;
    c.mtimeStart = FileMtimeMs(a.settingsFile);

    CapOptions opt;
    opt.nameFilter = a.nameFilter;
    opt.reqW = a.reqW;
    opt.reqH = a.reqH;
    opt.wantNv12 = a.nv12;
    opt.numFrames = a.frames;
    opt.intervalMs = a.intervalMs;
    opt.deadlineMs = (ULONGLONG)(a.frames * (std::max)(a.intervalMs, 1) + 60000);

    int rc = RunCaptureSession(opt, HotswitchCallback, &c, nullptr, nullptr);

    int uniqueTotal = 0;
    {
        std::vector<std::wstring> hashes;
        for (const auto& r : c.recs) hashes.push_back(r.hash);
        std::sort(hashes.begin(), hashes.end());
        uniqueTotal = (int)(std::unique(hashes.begin(), hashes.end()) - hashes.begin());
    }
    int uniqueBefore = 0, uniqueAfter = 0;
    if (c.switchFrame >= 0) {
        std::vector<std::wstring> hb, ha;
        for (const auto& r : c.recs) {
            if (r.index < c.switchFrame) hb.push_back(r.hash);
            else ha.push_back(r.hash);
        }
        std::sort(hb.begin(), hb.end());
        uniqueBefore = (int)(std::unique(hb.begin(), hb.end()) - hb.begin());
        std::sort(ha.begin(), ha.end());
        uniqueAfter = (int)(std::unique(ha.begin(), ha.end()) - ha.begin());
    }
    int blackCount = 0;
    for (const auto& r : c.recs) if (r.black) ++blackCount;

    const wchar_t* verdictStr;
    if (c.switchFrame >= 0 && uniqueAfter != uniqueBefore) verdictStr = L"SWITCHED";
    else if (c.switchFrame >= 0) verdictStr = L"SWITCH_NO_EFFECT";
    else verdictStr = L"NO_SWITCH";

    LogW(L"--- hotswitch summary: frames=%d switchFrame=%d uniqueBefore=%d uniqueAfter=%d uniqueTotal=%d black=%d verdict=%s",
         (int)c.recs.size(), c.switchFrame, uniqueBefore, uniqueAfter, uniqueTotal, blackCount, verdictStr);

    if (a.json) {
        wprintf(L"{\"mode\":\"hotswitch\",\"frames\":%d,\"switchFrame\":%d,"
                L"\"uniqueBefore\":%d,\"uniqueAfter\":%d,\"uniqueTotal\":%d,\"black\":%d,"
                L"\"verdict\":\"%s\"}\n",
                (int)c.recs.size(), c.switchFrame, uniqueBefore, uniqueAfter, uniqueTotal,
                blackCount, verdictStr);
    }
    if (rc == 0 && c.switchFrame >= 0 && uniqueAfter != uniqueBefore) return 0;
    return rc != 0 ? rc : 1;
}