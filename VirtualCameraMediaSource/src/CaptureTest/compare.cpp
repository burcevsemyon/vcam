#include "CaptureTestPCH.h"
#include "CaptureTestTypes.h"
#include "CaptureTestCommon.h"

#include "../Common/SharedMemoryContract.h"

struct CompareCtx {
    int matches = 0;
    int mismatches = 0;
    int shmFail = 0;
    int blackProxy = 0;
    int blackShm = 0;
    double threshold = 40.0;
};

static bool ReadShmFrame(BYTE* dst, DWORD dstCap, UINT32* pW, UINT32* pH, UINT32* pStride)
{
    HANDLE hMap = OpenFileMappingW(FILE_MAP_READ, FALSE, vcam::VCamSectionName);
    if (!hMap) return false;
    void* pView = MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, 0);
    if (!pView) { CloseHandle(hMap); return false; }
    auto* hdr = (const vcam::VCamSectionHeader*)pView;
    if (hdr->magic != vcam::VCamMagic || hdr->version != vcam::VCamVersion ||
        hdr->pixelFormat != (UINT32)vcam::VCamPixelFormat::RGB32) {
        UnmapViewOfFile(pView); CloseHandle(hMap); return false;
    }
    UINT32 w = hdr->width, h = hdr->height, stride = hdr->stride;
    UINT32 frameSize = hdr->frameSize;
    if (w != vcam::VCamWidth || h != vcam::VCamHeight || stride != vcam::VCamStride ||
        frameSize != vcam::VCamFrameSize || frameSize > dstCap) {
        UnmapViewOfFile(pView); CloseHandle(hMap); return false;
    }
    LONGLONG seq1 = hdr->seq;
    if (seq1 & 1) { UnmapViewOfFile(pView); CloseHandle(hMap); return false; }
    DWORD slotCount = hdr->slotCount;
    DWORD idx = hdr->frameWriteIndex % slotCount;
    const BYTE* src = (const BYTE*)pView + sizeof(vcam::VCamSectionHeader) + (SIZE_T)idx * vcam::VCamFrameSize;
    memcpy(dst, src, frameSize);
    LONGLONG seq2 = hdr->seq;
    UnmapViewOfFile(pView);
    CloseHandle(hMap);
    if (seq1 != seq2) return false;
    *pW = w; *pH = h; *pStride = stride;
    return true;
}

static void CompareCallback(void* ctx, const FrameCtx& fc)
{
    CompareCtx* c = (CompareCtx*)ctx;
    BYTE shmFrame[vcam::VCamFrameSize];
    UINT32 w = 0, h = 0, stride = 0;
    if (!ReadShmFrame(shmFrame, sizeof(shmFrame), &w, &h, &stride)) {
        ++c->shmFail;
        return;
    }
    int pw = (int)fc.s0.dwWidth, ph = (int)fc.s0.dwHeight;
    int pstride = (fc.s0.sub == MFVideoFormat_NV12) ? pw : (int)(fc.s0.dwWidth * 4);
    if ((int)w != pw || (int)h != ph || (int)stride != pstride) {
        ++c->mismatches;
        return;
    }
    if (memcmp(fc.pBits, shmFrame, fc.curLen) == 0) {
        ++c->matches;
    } else {
        ++c->mismatches;
    }
    PixelStats ps = ComputePixelStats(fc.pBits, fc.curLen, pw, ph, pstride, c->threshold);
    if (ps.mean < c->threshold) ++c->blackProxy;
    PixelStats ss = ComputePixelStats(shmFrame, fc.curLen, (int)w, (int)h, (int)stride, c->threshold);
    if (ss.mean < c->threshold) ++c->blackShm;
}

int RunCompareMode(const ModeArgs& a)
{
    SetLogFile(a.logFile);
    CompareCtx c;
    c.threshold = a.threshold;

    CapOptions opt;
    opt.nameFilter = a.nameFilter;
    opt.reqW = a.reqW;
    opt.reqH = a.reqH;
    opt.wantNv12 = a.nv12;
    opt.numFrames = a.frames;
    opt.intervalMs = a.intervalMs;
    opt.deadlineMs = (ULONGLONG)(a.frames * (std::max)(a.intervalMs, 1) + 30000);

    int rc = RunCaptureSession(opt, CompareCallback, &c, nullptr, nullptr);

    int total = c.matches + c.mismatches;
    const wchar_t* verdict = (total > 0 && c.mismatches == 0 && c.shmFail == 0) ? L"OK" : L"PROXY_DEFECT";

    LogW(L"--- compare summary: frames=%d matches=%d mismatches=%d shmFail=%d blackProxy=%d blackShm=%d verdict=%s",
         total, c.matches, c.mismatches, c.shmFail, c.blackProxy, c.blackShm, verdict);

    if (a.json) {
        wprintf(L"{\"mode\":\"compare\",\"frames\":%d,\"matches\":%d,\"mismatches\":%d,\"shmFail\":%d,\"blackProxy\":%d,\"blackShm\":%d,\"verdict\":\"%s\"}\n",
                total, c.matches, c.mismatches, c.shmFail, c.blackProxy, c.blackShm, verdict);
    }

    if (rc == 0 && c.mismatches == 0 && c.shmFail == 0) return 0;
    return rc != 0 ? rc : 1;
}