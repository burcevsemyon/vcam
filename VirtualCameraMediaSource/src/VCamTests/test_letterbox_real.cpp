#include "doctest.h"
#include <windows.h>

#include "Letterbox.h"

#include <cstring>
#include <vector>

// Тестируем РЕАЛЬНЫЙ код из Common/Letterbox.h (не копию).
// MediaStream.cpp и V2FrameReader.h используют vcam::LetterboxNearestFit.

static bool IsBlackRow(const BYTE* row, uint32_t w)
{
    for (uint32_t x = 0; x < w * 4; x++)
        if (row[x] != 0) return false;
    return true;
}

TEST_CASE("letterbox: 16:9 -> 4:3 has top/bottom bars")
{
    const uint32_t sw = 128, sh = 72, tw = 64, th = 48;
    const uint32_t ss = sw * 4, ds = tw * 4;
    std::vector<BYTE> src((size_t)ss * sh, 0xFF);
    std::vector<BYTE> dst((size_t)ds * th, 0xAB);

    vcam::LetterboxNearestFit(src.data(), ss, sw, sh, dst.data(), ds, tw, th);

    for (uint32_t y = 0; y < 5; y++)
        CHECK(IsBlackRow(&dst[(size_t)y * ds], tw));
    for (uint32_t y = 43; y < th; y++)
        CHECK(IsBlackRow(&dst[(size_t)y * ds], tw));
    bool hasContent = false;
    for (uint32_t x = 0; x < tw * 4; x += 4)
        if (dst[(size_t)(th / 2) * ds + x] != 0) { hasContent = true; break; }
    CHECK(hasContent);
}

TEST_CASE("letterbox: 4:3 -> 16:9 has left/right bars")
{
    const uint32_t sw = 64, sh = 48, tw = 128, th = 72;
    const uint32_t ss = sw * 4, ds = tw * 4;
    std::vector<BYTE> src((size_t)ss * sh, 0xFF);
    std::vector<BYTE> dst((size_t)ds * th, 0);

    vcam::LetterboxNearestFit(src.data(), ss, sw, sh, dst.data(), ds, tw, th);

    for (uint32_t y = 0; y < th; y += 4)
        for (uint32_t x = 0; x < 15 * 4; x += 4)
            CHECK(dst[(size_t)y * ds + x] == 0);
}

TEST_CASE("letterbox: same aspect = no bars")
{
    const uint32_t sw = 64, sh = 48, tw = 32, th = 24;
    const uint32_t ss = sw * 4, ds = tw * 4;
    std::vector<BYTE> src((size_t)ss * sh, 0xFF);
    std::vector<BYTE> dst((size_t)ds * th, 0);

    vcam::LetterboxNearestFit(src.data(), ss, sw, sh, dst.data(), ds, tw, th);

    bool topContent = false;
    for (uint32_t x = 0; x < tw * 4; x += 4)
        if (dst[x] != 0) { topContent = true; break; }
    CHECK(topContent);
}

TEST_CASE("letterbox: exact same size is 1:1 copy")
{
    const uint32_t w = 32, h = 24;
    const uint32_t s = w * 4;
    std::vector<BYTE> src((size_t)s * h);
    for (size_t i = 0; i < src.size(); i++) src[i] = (BYTE)(i & 0xFF);
    std::vector<BYTE> dst((size_t)s * h, 0);

    vcam::LetterboxNearestFit(src.data(), s, w, h, dst.data(), s, w, h);
    CHECK(std::memcmp(dst.data(), src.data(), src.size()) == 0);
}

TEST_CASE("letterbox: 1-pixel source to large target")
{
    const uint32_t sw = 1, sh = 1, tw = 8, th = 8;
    const uint32_t ss = 4, ds = tw * 4;
    std::vector<BYTE> src(4, 0xFF);
    std::vector<BYTE> dst((size_t)ds * th, 0);

    vcam::LetterboxNearestFit(src.data(), ss, sw, sh, dst.data(), ds, tw, th);
    bool anyContent = false;
    for (size_t i = 0; i < dst.size(); i += 4)
        if (dst[i] != 0) { anyContent = true; break; }
    CHECK(anyContent);
}

TEST_CASE("letterbox: extreme wide source = pillarbox")
{
    const uint32_t sw = 1920, sh = 100, tw = 64, th = 48;
    const uint32_t ss = sw * 4, ds = tw * 4;
    std::vector<BYTE> src((size_t)ss * sh, 0xFF);
    std::vector<BYTE> dst((size_t)ds * th, 0);

    vcam::LetterboxNearestFit(src.data(), ss, sw, sh, dst.data(), ds, tw, th);
    CHECK(IsBlackRow(&dst[0], tw / 4));
}

TEST_CASE("letterbox: extreme tall source = letterbox")
{
    const uint32_t sw = 100, sh = 1920, tw = 640, th = 480;
    const uint32_t ss = sw * 4, ds = tw * 4;
    std::vector<BYTE> src((size_t)ss * sh, 0xFF);
    std::vector<BYTE> dst((size_t)ds * th, 0);

    vcam::LetterboxNearestFit(src.data(), ss, sw, sh, dst.data(), ds, tw, th);
    // Tall source -> pillarbox (outW = 100*480/1920 = 25, offX = (640-25)/2 = 307).
    CHECK(dst[(size_t)(th / 2) * ds] == 0);
    CHECK(dst[(size_t)(th / 2) * ds + 300 * 4] == 0);
}

TEST_CASE("letterbox: zero target is safe no-op")
{
    std::vector<BYTE> dst(64, 0xAB);
    vcam::LetterboxNearestFit(nullptr, 0, 0, 0, dst.data(), 0, 0, 0);
    CHECK(true);
}

TEST_CASE("letterbox: 720p -> 640x480 (ktalk path)")
{
    const uint32_t sw = 1280, sh = 720, tw = 640, th = 480;
    const uint32_t ss = sw * 4, ds = tw * 4;
    std::vector<BYTE> src((size_t)ss * sh, 0xFF);
    std::vector<BYTE> dst((size_t)ds * th, 0xAB);

    vcam::LetterboxNearestFit(src.data(), ss, sw, sh, dst.data(), ds, tw, th);

    // 16:9 -> 4:3: outH = 360, offY = 60.
    for (uint32_t y = 0; y < 55; y++)
        CHECK(IsBlackRow(&dst[(size_t)y * ds], tw));
    for (uint32_t y = 425; y < th; y++)
        CHECK(IsBlackRow(&dst[(size_t)y * ds], tw));
}
