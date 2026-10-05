#include "doctest.h"
#include <windows.h>
#include "SharedMemoryContract.h"

#include <string>

// ResolveV2Size — чистая математика лесенки quality (из FrameWriter.h).
// Копия логики для теста без тяжёлых зависимостей FrameWriter (MF/ATL/shm).
// При изменении FrameWriter.h ResolveV2Size — синхронизировать!
static bool ResolveV2Size(uint32_t srcW, uint32_t srcH, const std::wstring& quality,
                          uint32_t& outW, uint32_t& outH)
{
    if (srcW == 0 || srcH == 0 || srcW > 8192 || srcH > 8192) return false;
    if (quality == L"fixed720p") {
        outW = vcam::VCamWidth;
        outH = vcam::VCamHeight;
        return true;
    }
    if (quality == L"fixed1080p") {
        constexpr uint32_t capW = 1920, capH = 1080;
        if (srcW <= capW && srcH <= capH) { outW = srcW; outH = srcH; return true; }
        double s = (double)capW / srcW;
        double s2 = (double)capH / srcH;
        if (s2 < s) s = s2;
        outW = (uint32_t)(srcW * s + 0.5);
        outH = (uint32_t)(srcH * s + 0.5);
        if (outW < 1u) outW = 1;
        if (outH < 1u) outH = 1;
        if (outW > capW) outW = capW;
        if (outH > capH) outH = capH;
        return true;
    }
    // source: fit-clamp to cap 4K.
    if (srcW <= vcam::VCamNativeCapW && srcH <= vcam::VCamNativeCapH) {
        outW = srcW; outH = srcH; return true;
    }
    double s = (double)vcam::VCamNativeCapW / srcW;
    double s2 = (double)vcam::VCamNativeCapH / srcH;
    if (s2 < s) s = s2;
    outW = (uint32_t)(srcW * s + 0.5);
    outH = (uint32_t)(srcH * s + 0.5);
    if (outW < 1u) outW = 1;
    if (outH < 1u) outH = 1;
    if (outW > vcam::VCamNativeCapW) outW = vcam::VCamNativeCapW;
    if (outH > vcam::VCamNativeCapH) outH = vcam::VCamNativeCapH;
    return true;
}

TEST_CASE("quality: source passthrough under cap")
{
    uint32_t w = 0, h = 0;
    CHECK(ResolveV2Size(1280, 698, L"source", w, h));
    CHECK(w == 1280);
    CHECK(h == 698);
}

TEST_CASE("quality: source clamp at 4K cap")
{
    uint32_t w = 0, h = 0;
    CHECK(ResolveV2Size(3840, 2160, L"source", w, h));
    CHECK(w == 3840);
    CHECK(h == 2160);
}

TEST_CASE("quality: source over cap scales down preserving aspect")
{
    uint32_t w = 0, h = 0;
    CHECK(ResolveV2Size(4000, 3000, L"source", w, h));
    CHECK(w <= 3840);
    CHECK(h <= 2160);
    CHECK(w > 0);
    CHECK(h > 0);
}

TEST_CASE("quality: fixed720p always 1280x720")
{
    uint32_t w = 0, h = 0;
    CHECK(ResolveV2Size(3840, 2160, L"fixed720p", w, h));
    CHECK(w == 1280);
    CHECK(h == 720);
    CHECK(ResolveV2Size(320, 240, L"fixed720p", w, h));
    CHECK(w == 1280);
    CHECK(h == 720);
}

TEST_CASE("quality: fixed1080p passthrough under cap")
{
    uint32_t w = 0, h = 0;
    CHECK(ResolveV2Size(1280, 698, L"fixed1080p", w, h));
    CHECK(w == 1280); // under 1920x1080 -> passthrough
    CHECK(h == 698);
}

TEST_CASE("quality: fixed1080p exact cap passthrough")
{
    uint32_t w = 0, h = 0;
    CHECK(ResolveV2Size(1920, 1080, L"fixed1080p", w, h));
    CHECK(w == 1920);
    CHECK(h == 1080);
}

TEST_CASE("quality: fixed1080p over cap scales down")
{
    uint32_t w = 0, h = 0;
    CHECK(ResolveV2Size(3840, 2160, L"fixed1080p", w, h));
    CHECK(w == 1920);
    CHECK(h == 1080);
}

TEST_CASE("quality: fixed1080p wide source clamps by width")
{
    uint32_t w = 0, h = 0;
    CHECK(ResolveV2Size(4000, 2000, L"fixed1080p", w, h));
    CHECK(w == 1920);
    CHECK(h == 960); // 2000 * 1920/4000 = 960
}

TEST_CASE("quality: fixed1080p tall source clamps by height")
{
    uint32_t w = 0, h = 0;
    CHECK(ResolveV2Size(1000, 3000, L"fixed1080p", w, h));
    CHECK(h == 1080);
    CHECK(w == 360); // 1000 * 1080/3000 = 360
}

TEST_CASE("quality: garbage input rejected")
{
    uint32_t w = 0, h = 0;
    CHECK(!ResolveV2Size(0, 720, L"source", w, h));
    CHECK(!ResolveV2Size(1280, 0, L"source", w, h));
    CHECK(!ResolveV2Size(10000, 720, L"source", w, h));
    CHECK(!ResolveV2Size(1280, 10000, L"source", w, h));
}

TEST_CASE("quality: unknown token falls back to source")
{
    uint32_t w = 0, h = 0;
    CHECK(ResolveV2Size(1280, 720, L"bogus", w, h));
    CHECK(w == 1280);
    CHECK(h == 720);
}
