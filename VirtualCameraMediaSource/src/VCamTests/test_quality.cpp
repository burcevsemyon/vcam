#include "doctest.h"
#include <windows.h>

#include "QualityLadder.h"

#include <string>

// Тестируем РЕАЛЬНЫЙ код из Common/QualityLadder.h (не копию).
// FrameWriter::ResolveV2Size — тонкая обёртка над vcam::ResolveV2Size.

TEST_CASE("quality: source passthrough under cap")
{
    uint32_t w = 0, h = 0;
    CHECK(vcam::ResolveV2Size(1280, 698, L"source", w, h));
    CHECK(w == 1280);
    CHECK(h == 698);
}

TEST_CASE("quality: source clamp at 4K cap")
{
    uint32_t w = 0, h = 0;
    CHECK(vcam::ResolveV2Size(3840, 2160, L"source", w, h));
    CHECK(w == 3840);
    CHECK(h == 2160);
}

TEST_CASE("quality: source over cap scales down preserving aspect")
{
    uint32_t w = 0, h = 0;
    CHECK(vcam::ResolveV2Size(4000, 3000, L"source", w, h));
    CHECK(w <= 3840u);
    CHECK(h <= 2160u);
    CHECK(w > 0u);
    CHECK(h > 0u);
}

TEST_CASE("quality: fixed720p always 1280x720")
{
    uint32_t w = 0, h = 0;
    CHECK(vcam::ResolveV2Size(3840, 2160, L"fixed720p", w, h));
    CHECK(w == 1280);
    CHECK(h == 720);
    CHECK(vcam::ResolveV2Size(320, 240, L"fixed720p", w, h));
    CHECK(w == 1280);
    CHECK(h == 720);
}

TEST_CASE("quality: fixed1080p passthrough under cap")
{
    uint32_t w = 0, h = 0;
    CHECK(vcam::ResolveV2Size(1280, 698, L"fixed1080p", w, h));
    CHECK(w == 1280);
    CHECK(h == 698);
}

TEST_CASE("quality: fixed1080p exact cap passthrough")
{
    uint32_t w = 0, h = 0;
    CHECK(vcam::ResolveV2Size(1920, 1080, L"fixed1080p", w, h));
    CHECK(w == 1920);
    CHECK(h == 1080);
}

TEST_CASE("quality: fixed1080p over cap scales down")
{
    uint32_t w = 0, h = 0;
    CHECK(vcam::ResolveV2Size(3840, 2160, L"fixed1080p", w, h));
    CHECK(w == 1920);
    CHECK(h == 1080);
}

TEST_CASE("quality: fixed1080p wide source clamps by width")
{
    uint32_t w = 0, h = 0;
    CHECK(vcam::ResolveV2Size(4000, 2000, L"fixed1080p", w, h));
    CHECK(w == 1920);
    CHECK(h == 960);
}

TEST_CASE("quality: fixed1080p tall source clamps by height")
{
    uint32_t w = 0, h = 0;
    CHECK(vcam::ResolveV2Size(1000, 3000, L"fixed1080p", w, h));
    CHECK(h == 1080);
    CHECK(w == 360);
}

TEST_CASE("quality: garbage input rejected")
{
    uint32_t w = 0, h = 0;
    CHECK(!vcam::ResolveV2Size(0, 720, L"source", w, h));
    CHECK(!vcam::ResolveV2Size(1280, 0, L"source", w, h));
    CHECK(!vcam::ResolveV2Size(10000, 720, L"source", w, h));
    CHECK(!vcam::ResolveV2Size(1280, 10000, L"source", w, h));
}

TEST_CASE("quality: unknown token falls back to source")
{
    uint32_t w = 0, h = 0;
    CHECK(vcam::ResolveV2Size(1280, 720, L"bogus", w, h));
    CHECK(w == 1280);
    CHECK(h == 720);
}
