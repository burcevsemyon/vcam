#include "doctest.h"
#include <windows.h>
#include "ImageLayout.h"

#include <cstring>
#include <vector>

// ImageLayout.h — чистая математика layout'а кадра. Тестируем:
// 1. LetterboxBilinearEx: 16:9 → 4:3 даёт чёрные полосы сверху/снизу.
// 2. CoverBilinearEx: заполняет весь dst без полос.
// 3. StretchBilinearEx: без полос, весь dst перезаписан.
// 4. Вырожденные случаи (sw=1, sh=1).

static bool IsBlackRow(const BYTE* row, UINT w)
{
    for (UINT x = 0; x < w * 4; x++)
        if (row[x] != 0) return false;
    return true;
}

static bool IsBlackAt(const BYTE* frame, UINT stride, UINT x, UINT y)
{
    const BYTE* p = frame + (size_t)y * stride + (size_t)x * 4;
    return p[0] == 0 && p[1] == 0 && p[2] == 0 && p[3] == 0;
}

TEST_CASE("imagelayout: LetterboxBilinearEx 16:9 -> 4:3 has black bars")
{
    const UINT sw = 128, sh = 72, tw = 64, th = 48;
    const LONG ss = sw * 4, ds = tw * 4;
    std::vector<BYTE> src((size_t)ss * sh, 0xFF); // solid white
    std::vector<BYTE> dst((size_t)ds * th, 0xAB); // garbage

    vcam::LetterboxBilinearEx(src.data(), sw, sh, ss, dst.data(), tw, th, ds);

    // 16:9 -> 4:3: outH = 72 * 64 / 128 = 36, offY = (48-36)/2 = 6.
    // Row 0..5 must be black (top bar), row 6..41 content, row 42..47 black.
    for (UINT y = 0; y < 5; y++)
        CHECK(IsBlackRow(&dst[(size_t)y * ds], tw));
    for (UINT y = 43; y < th; y++)
        CHECK(IsBlackRow(&dst[(size_t)y * ds], tw));
    // Center row must have content (not black).
    bool hasContent = false;
    for (UINT x = 0; x < tw; x++)
        if (!IsBlackAt(dst.data(), ds, x, th / 2)) { hasContent = true; break; }
    CHECK(hasContent);
}

TEST_CASE("imagelayout: CoverBilinearEx fills entire dst (no bars)")
{
    const UINT sw = 128, sh = 72, tw = 64, th = 48;
    const LONG ss = sw * 4, ds = tw * 4;
    std::vector<BYTE> src((size_t)ss * sh, 0xFF);
    std::vector<BYTE> dst((size_t)ds * th, 0);

    vcam::CoverBilinearEx(src.data(), sw, sh, ss, dst.data(), tw, th, ds);

    // Cover: no bars — every row must have content.
    for (UINT y = 0; y < th; y += 4)
    {
        bool rowContent = false;
        for (UINT x = 0; x < tw; x += 4)
            if (!IsBlackAt(dst.data(), ds, x, y)) { rowContent = true; break; }
        CHECK(rowContent);
    }
}

TEST_CASE("imagelayout: StretchBilinearEx fills entire dst")
{
    const UINT sw = 64, sh = 48, tw = 32, th = 24;
    const LONG ss = sw * 4, ds = tw * 4;
    std::vector<BYTE> src((size_t)ss * sh, 0xFF);
    std::vector<BYTE> dst((size_t)ds * th, 0);

    vcam::StretchBilinearEx(src.data(), sw, sh, ss, dst.data(), tw, th, ds);

    for (UINT y = 0; y < th; y += 2)
    {
        bool rowContent = false;
        for (UINT x = 0; x < tw; x += 2)
            if (!IsBlackAt(dst.data(), ds, x, y)) { rowContent = true; break; }
        CHECK(rowContent);
    }
}

TEST_CASE("imagelayout: LetterboxBilinearEx same aspect has no bars")
{
    const UINT sw = 64, sh = 48, tw = 32, th = 24; // both 4:3
    const LONG ss = sw * 4, ds = tw * 4;
    std::vector<BYTE> src((size_t)ss * sh, 0xFF);
    std::vector<BYTE> dst((size_t)ds * th, 0);

    vcam::LetterboxBilinearEx(src.data(), sw, sh, ss, dst.data(), tw, th, ds);

    // Same aspect = no bars. Check top row has content.
    bool topContent = false;
    for (UINT x = 0; x < tw; x++)
        if (!IsBlackAt(dst.data(), ds, x, 0)) { topContent = true; break; }
    CHECK(topContent);
}

TEST_CASE("imagelayout: degenerate 1-pixel source")
{
    const UINT sw = 1, sh = 1, tw = 8, th = 8;
    const LONG ss = 4, ds = tw * 4;
    std::vector<BYTE> src(4, 0xFF);
    std::vector<BYTE> dst((size_t)ds * th, 0);

    vcam::LetterboxBilinearEx(src.data(), sw, sh, ss, dst.data(), tw, th, ds);
    // 1x1 -> 8x8 same aspect: should fill (no crash, some content).
    bool anyContent = false;
    for (UINT y = 0; y < th && !anyContent; y++)
        for (UINT x = 0; x < tw; x++)
            if (!IsBlackAt(dst.data(), ds, x, y)) { anyContent = true; break; }
    CHECK(anyContent);
}

TEST_CASE("imagelayout: zero-size target is safe no-op")
{
    std::vector<BYTE> dst(64, 0xAB);
    vcam::LetterboxBilinearEx(nullptr, 0, 0, 0, dst.data(), 0, 0, 0);
    // No crash = pass.
    CHECK(true);
}
