#include "doctest.h"
#include <windows.h>

#include <cstring>
#include <vector>

// DownsampleRgb32 — letterbox-fit даунскейл RGB32 из MediaStream.cpp
// (v1-путь: 720p -> 640x480 через прокси FrameServer = путь ktalk).
// Копия логики для теста без MF/COM зависимостей.
// При изменении MediaStream.cpp DownsampleRgb32 — синхронизировать!
// ОСОБЕННОСТЬ: нет zero-guard для outW/outH (в отличие от V2FrameReader).
static void DownsampleRgb32(const BYTE* pSrc, BYTE* pDst,
                            UINT32 srcW, UINT32 srcH, UINT32 dstW, UINT32 dstH,
                            UINT32 srcStride)
{
    UINT32 outW = dstW, outH = dstH;
    if ((UINT64)srcW * dstH > (UINT64)dstW * srcH) {
        outH = (UINT32)((UINT64)srcH * dstW / srcW);
    } else {
        outW = (UINT32)((UINT64)srcW * dstH / srcH);
    }
    const UINT32 offX = (dstW - outW) / 2;
    const UINT32 offY = (dstH - outH) / 2;

    memset(pDst, 0, (SIZE_T)dstW * dstH * 4);
    for (UINT32 y = 0; y < outH; ++y) {
        UINT32 srcY = (UINT32)((UINT64)y * srcH / outH);
        const BYTE* pSrcRow = pSrc + (SIZE_T)srcY * srcStride;
        BYTE* pDstRow = pDst + (SIZE_T)(y + offY) * (dstW * 4) + (SIZE_T)offX * 4;
        for (UINT32 x = 0; x < outW; ++x) {
            UINT32 srcX = (UINT32)((UINT64)x * srcW / outW);
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

// Основной кейс ktalk: 1280x720 (16:9) -> 640x480 (4:3).
TEST_CASE("v1downsample: 720p -> 640x480 letterbox bars")
{
    const UINT32 sw = 1280, sh = 720, tw = 640, th = 480;
    const UINT32 ss = sw * 4, ds = tw * 4;
    std::vector<BYTE> src((SIZE_T)ss * sh, 0xFF);
    std::vector<BYTE> dst((SIZE_T)ds * th, 0xAB);

    DownsampleRgb32(src.data(), dst.data(), sw, sh, tw, th, ss);

    // 16:9 -> 4:3: outH = 720*640/1280 = 360, offY = (480-360)/2 = 60.
    for (UINT32 y = 0; y < 55; y++)
        CHECK(IsBlackRow(&dst[(SIZE_T)y * ds], tw));
    for (UINT32 y = 425; y < th; y++)
        CHECK(IsBlackRow(&dst[(SIZE_T)y * ds], tw));
    // Center has content.
    bool hasContent = false;
    for (UINT32 x = 0; x < tw; x += 8)
        if (dst[(SIZE_T)(th / 2) * ds + x * 4] != 0) { hasContent = true; break; }
    CHECK(hasContent);
}

TEST_CASE("v1downsample: 4:3 -> 4:3 no bars")
{
    const UINT32 sw = 640, sh = 480, tw = 320, th = 240;
    const UINT32 ss = sw * 4, ds = tw * 4;
    std::vector<BYTE> src((SIZE_T)ss * sh, 0xFF);
    std::vector<BYTE> dst((SIZE_T)ds * th, 0);

    DownsampleRgb32(src.data(), dst.data(), sw, sh, tw, th, ss);

    bool topContent = false;
    for (UINT32 x = 0; x < tw; x += 4)
        if (dst[x * 4] != 0) { topContent = true; break; }
    CHECK(topContent);
}

TEST_CASE("v1downsample: 16:9 -> 16:9 no bars")
{
    const UINT32 sw = 1280, sh = 720, tw = 640, th = 360;
    const UINT32 ss = sw * 4, ds = tw * 4;
    std::vector<BYTE> src((SIZE_T)ss * sh, 0xFF);
    std::vector<BYTE> dst((SIZE_T)ds * th, 0);

    DownsampleRgb32(src.data(), dst.data(), sw, sh, tw, th, ss);

    bool topContent = false;
    for (UINT32 x = 0; x < tw; x += 8)
        if (dst[x * 4] != 0) { topContent = true; break; }
    CHECK(topContent);
}

TEST_CASE("v1downsample: extreme aspect ratio doesn't crash")
{
    const UINT32 sw = 1920, sh = 100, tw = 64, th = 48; // very wide source
    const UINT32 ss = sw * 4, ds = tw * 4;
    std::vector<BYTE> src((SIZE_T)ss * sh, 0xFF);
    std::vector<BYTE> dst((SIZE_T)ds * th, 0);

    DownsampleRgb32(src.data(), dst.data(), sw, sh, tw, th, ss);
    // Wide source -> pillarbox (left/right bars).
    CHECK(IsBlackRow(&dst[0], tw / 4)); // left edge black
}

TEST_CASE("v1downsample: tall source to wide target = pillarbox")
{
    const UINT32 sw = 100, sh = 1920, tw = 640, th = 480;
    const UINT32 ss = sw * 4, ds = tw * 4;
    std::vector<BYTE> src((SIZE_T)ss * sh, 0xFF);
    std::vector<BYTE> dst((SIZE_T)ds * th, 0);

    DownsampleRgb32(src.data(), dst.data(), sw, sh, tw, th, ss);
    // Tall source (aspect 0.052) -> pillarbox: outW = 100*480/1920 = 25,
    // offX = (640-25)/2 = 307. Left 307 px of center row are black.
    CHECK(dst[(SIZE_T)(th / 2) * ds + 0] == 0); // left edge
    CHECK(dst[(SIZE_T)(th / 2) * ds + 300 * 4] == 0); // still in bar
    // Center has content (25 px wide image at x=307..332).
    bool hasContent = false;
    for (UINT32 x = 310; x < 330; x++)
        if (dst[(SIZE_T)(th / 2) * ds + x * 4] != 0) { hasContent = true; break; }
    CHECK(hasContent);
}
