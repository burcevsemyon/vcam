#include "doctest.h"
#include <windows.h>

#include <cstring>
#include <vector>

// DownscaleRgb32Letterbox — letterbox-fit даунскейл/апскейл RGB32
// nearest-neighbour (из V2FrameReader.h, namespace vcam_v2).
// Копия логики для теста без ATL/MappedViewOfFilePtr зависимостей.
// При изменении V2FrameReader.h DownscaleRgb32Letterbox — синхронизировать!
static void DownscaleRgb32Letterbox(const BYTE* pSrc, SIZE_T srcStride, UINT32 srcW, UINT32 srcH,
                                    BYTE* pDst, SIZE_T dstStride, UINT32 dstW, UINT32 dstH)
{
    UINT32 outW = dstW, outH = dstH;
    if ((UINT64)srcW * dstH > (UINT64)dstW * srcH) {
        outH = (UINT32)((UINT64)srcH * dstW / srcW);
        if (outH == 0) outH = 1;
    } else {
        outW = (UINT32)((UINT64)srcW * dstH / srcH);
        if (outW == 0) outW = 1;
    }
    const UINT32 offX = (dstW - outW) / 2;
    const UINT32 offY = (dstH - outH) / 2;

    for (UINT32 y = 0; y < dstH; ++y)
        memset(pDst + (SIZE_T)y * dstStride, 0, (SIZE_T)dstW * 4);
    for (UINT32 y = 0; y < outH; ++y) {
        const UINT32 srcY = (UINT32)((UINT64)y * srcH / outH);
        const BYTE* pSrcRow = pSrc + (SIZE_T)srcY * srcStride;
        BYTE* pDstRow = pDst + (SIZE_T)(y + offY) * dstStride + (SIZE_T)offX * 4;
        for (UINT32 x = 0; x < outW; ++x) {
            const UINT32 srcX = (UINT32)((UINT64)x * srcW / outW);
            const BYTE* pSrcPixel = pSrcRow + (SIZE_T)srcX * 4;
            BYTE* pDstPixel = pDstRow + (SIZE_T)x * 4;
            pDstPixel[0] = pSrcPixel[0];
            pDstPixel[1] = pSrcPixel[1];
            pDstPixel[2] = pSrcPixel[2];
            pDstPixel[3] = pSrcPixel[3];
        }
    }
}

static bool IsBlackRow(const BYTE* row, UINT32 w)
{
    for (UINT32 x = 0; x < w * 4; x++)
        if (row[x] != 0) return false;
    return true;
}

TEST_CASE("v2downscale: 16:9 -> 4:3 has top/bottom bars")
{
    const UINT32 sw = 128, sh = 72, tw = 64, th = 48;
    const SIZE_T ss = sw * 4, ds = tw * 4;
    std::vector<BYTE> src(ss * sh, 0xFF);
    std::vector<BYTE> dst(ds * th, 0xAB);

    DownscaleRgb32Letterbox(src.data(), ss, sw, sh, dst.data(), ds, tw, th);

    // 16:9 -> 4:3: outH = 72*64/128 = 36, offY = (48-36)/2 = 6.
    for (UINT32 y = 0; y < 6; y++)
        CHECK(IsBlackRow(&dst[y * ds], tw));
    for (UINT32 y = 42; y < th; y++)
        CHECK(IsBlackRow(&dst[y * ds], tw));
    // Center has content.
    bool hasContent = false;
    for (UINT32 x = 0; x < tw * 4; x += 4)
        if (dst[th / 2 * ds + x] != 0) { hasContent = true; break; }
    CHECK(hasContent);
}

TEST_CASE("v2downscale: 4:3 -> 16:9 has left/right bars")
{
    const UINT32 sw = 64, sh = 48, tw = 128, th = 72;
    const SIZE_T ss = sw * 4, ds = tw * 4;
    std::vector<BYTE> src(ss * sh, 0xFF);
    std::vector<BYTE> dst(ds * th, 0);

    DownscaleRgb32Letterbox(src.data(), ss, sw, sh, dst.data(), ds, tw, th);

    // 4:3 -> 16:9: outW = 64*72/48 = 96, offX = (128-96)/2 = 16.
    // Left 16 pixels of each row should be black.
    for (UINT32 y = 0; y < th; y += 4) {
        for (UINT32 x = 0; x < 15 * 4; x += 4)
            CHECK(dst[y * ds + x] == 0);
    }
}

TEST_CASE("v2downscale: same aspect = no bars")
{
    const UINT32 sw = 64, sh = 48, tw = 32, th = 24; // both 4:3
    const SIZE_T ss = sw * 4, ds = tw * 4;
    std::vector<BYTE> src(ss * sh, 0xFF);
    std::vector<BYTE> dst(ds * th, 0);

    DownscaleRgb32Letterbox(src.data(), ss, sw, sh, dst.data(), ds, tw, th);

    // Same aspect: top row has content (no bar).
    bool topContent = false;
    for (UINT32 x = 0; x < tw * 4; x += 4)
        if (dst[x] != 0) { topContent = true; break; }
    CHECK(topContent);
}

TEST_CASE("v2downscale: upscale adds bars not crash")
{
    const UINT32 sw = 16, sh = 9, tw = 64, th = 36;
    const SIZE_T ss = sw * 4, ds = tw * 4;
    std::vector<BYTE> src(ss * sh, 0xFF);
    std::vector<BYTE> dst(ds * th, 0);

    DownscaleRgb32Letterbox(src.data(), ss, sw, sh, dst.data(), ds, tw, th);

    // 16:9 -> 16:9 upscale: no bars, content everywhere.
    for (UINT32 y = 0; y < th; y += 8) {
        bool rowContent = false;
        for (UINT32 x = 0; x < tw * 4; x += 16)
            if (dst[y * ds + x] != 0) { rowContent = true; break; }
        CHECK(rowContent);
    }
}

TEST_CASE("v2downscale: 1-pixel source to large target")
{
    const UINT32 sw = 1, sh = 1, tw = 8, th = 8;
    const SIZE_T ss = 4, ds = tw * 4;
    std::vector<BYTE> src(4, 0xFF);
    std::vector<BYTE> dst(ds * th, 0);

    DownscaleRgb32Letterbox(src.data(), ss, sw, sh, dst.data(), ds, tw, th);
    // 1x1 -> 8x8: same aspect, fills (no crash).
    bool anyContent = false;
    for (SIZE_T i = 0; i < dst.size(); i += 4)
        if (dst[i] != 0) { anyContent = true; break; }
    CHECK(anyContent);
}

TEST_CASE("v2downscale: exact same size is 1:1 copy")
{
    const UINT32 w = 32, h = 24;
    const SIZE_T s = w * 4;
    std::vector<BYTE> src(s * h);
    for (size_t i = 0; i < src.size(); i++) src[i] = (BYTE)(i & 0xFF);
    std::vector<BYTE> dst(s * h, 0);

    DownscaleRgb32Letterbox(src.data(), s, w, h, dst.data(), s, w, h);
    CHECK(std::memcmp(dst.data(), src.data(), src.size()) == 0);
}
