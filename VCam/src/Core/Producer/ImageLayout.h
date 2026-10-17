#pragma once

#include <cmath>
#include <cstring>
#include <vector>

#include "SharedMemoryContract.h"

// Общий модуль layout'а кадра (бывшее дублирование в CameraSource.cpp и
// VideoFileSource.cpp): RowPtr + LetterboxNearest/LetterboxBilinear.
// Сигнатура с явным dstride — общий знаменатель обеих реализаций
// (VideoFileSource всегда вызывал с vcam::VCamStride, CameraSource — с
// stride'ом целевого буфера); формулы/пиксели не менялись.
//
// v2 (фаза vcam-quality-v2/sub1): параметризованные Ex-варианты под
// произвольный target (натив источника, cap 4K). Старые LetterboxNearest /
// LetterboxBilinear — тонкие обёртки над Ex с 1280x720: v1-путь бит-в-бит
// (равенство доказывает harness: memcmp old vs Ex@720p == 0).

namespace vcam {

inline const BYTE* RowPtr(const BYTE* data, LONG stride, UINT h, UINT y)
{
    if (stride >= 0) return data + (size_t)y * (size_t)stride;
    return data + (size_t)(h - 1 - y) * (size_t)(-stride);
}

inline void LetterboxNearestEx(const BYTE* src, UINT sw, UINT sh, LONG sstride,
                               BYTE* dst, UINT tw, UINT th, LONG dstride)
{
    if (tw == 0 || th == 0) return;
    memset(dst, 0, (size_t)dstride * th);
    if (sw == 0 || sh == 0) return;
    double scale = (double)tw / sw;
    if ((double)th / sh < scale) scale = (double)th / sh;
    int dw = (int)llround((double)sw * scale);
    int dh = (int)llround((double)sh * scale);
    if (dw < 1) dw = 1;
    if (dh < 1) dh = 1;
    if (dw > (int)tw) dw = (int)tw;
    if (dh > (int)th) dh = (int)th;
    int x0 = ((int)tw - dw) / 2;
    int y0 = ((int)th - dh) / 2;

    for (int y = 0; y < dh; y++) {
        UINT sy = (UINT)((int64_t)y * sh / dh);
        if (sy >= sh) sy = sh - 1;
        const BYTE* row = RowPtr(src, sstride, sh, sy);
        BYTE* drow = dst + (size_t)(y0 + y) * dstride + (size_t)x0 * 4;
        for (int x = 0; x < dw; x++) {
            UINT sx = (UINT)((int64_t)x * sw / dw);
            if (sx >= sw) sx = sw - 1;
            const BYTE* p = row + (size_t)sx * 4;
            BYTE* q = drow + (size_t)x * 4;
            q[0] = p[0]; q[1] = p[1]; q[2] = p[2]; q[3] = p[3];
        }
    }
}

inline void LetterboxNearest(const BYTE* src, UINT sw, UINT sh, LONG sstride,
                             BYTE* dst, LONG dstride)
{
    LetterboxNearestEx(src, sw, sh, sstride, dst,
                       vcam::VCamWidth, vcam::VCamHeight, dstride);
}

// Точный билинейный stretch sw,sh -> tw,th (без полей, весь dst перезаписан).
inline void StretchBilinearEx(const BYTE* src, UINT sw, UINT sh, LONG sstride,
                              BYTE* dst, UINT tw, UINT th, LONG dstride)
{
    if (tw == 0 || th == 0 || sw == 0 || sh == 0) return;
    if (sw < 2 || sh < 2) {
        // Вырожденный вход: nearest-растяжка (dst полностью покрыт, без memset).
        for (UINT y = 0; y < th; y++) {
            UINT sy = (sh == 1) ? 0 : (UINT)((int64_t)y * (sh - 1) / (th - 1 > 0 ? th - 1 : 1));
            if (sy >= sh) sy = sh - 1;
            const BYTE* row = RowPtr(src, sstride, sh, sy);
            BYTE* drow = dst + (size_t)y * dstride;
            for (UINT x = 0; x < tw; x++) {
                UINT sx = (sw == 1) ? 0 : (UINT)((int64_t)x * (sw - 1) / (tw - 1 > 0 ? tw - 1 : 1));
                if (sx >= sw) sx = sw - 1;
                const BYTE* p = row + (size_t)sx * 4;
                BYTE* q = drow + (size_t)x * 4;
                q[0] = p[0]; q[1] = p[1]; q[2] = p[2]; q[3] = p[3];
            }
        }
        return;
    }

    const int64_t stepX = ((int64_t)sw << 16) / tw;
    const int64_t stepY = ((int64_t)sh << 16) / th;
    const int64_t maxX = ((int64_t)(sw - 1) << 16);
    const int64_t maxY = ((int64_t)(sh - 1) << 16);

    for (UINT y = 0; y < th; y++) {
        int64_t sy16 = (int64_t)y * stepY + stepY / 2 - 32768;
        if (sy16 < 0) sy16 = 0;
        if (sy16 > maxY) sy16 = maxY;
        UINT sy0 = (UINT)(sy16 >> 16);
        UINT fy = (UINT)(sy16 & 0xFFFF);
        if (sy0 >= sh - 1) { sy0 = sh - 2; fy = 0xFFFF; }
        UINT sy1 = sy0 + 1;

        const BYTE* r0 = RowPtr(src, sstride, sh, sy0);
        const BYTE* r1 = RowPtr(src, sstride, sh, sy1);
        BYTE* drow = dst + (size_t)y * dstride;

        for (UINT x = 0; x < tw; x++) {
            int64_t sx16 = (int64_t)x * stepX + stepX / 2 - 32768;
            if (sx16 < 0) sx16 = 0;
            if (sx16 > maxX) sx16 = maxX;
            UINT sx0 = (UINT)(sx16 >> 16);
            UINT fx = (UINT)(sx16 & 0xFFFF);
            if (sx0 >= sw - 1) { sx0 = sw - 2; fx = 0xFFFF; }
            UINT sx1 = sx0 + 1;

            const BYTE* p00 = r0 + (size_t)sx0 * 4;
            const BYTE* p01 = r0 + (size_t)sx1 * 4;
            const BYTE* p10 = r1 + (size_t)sx0 * 4;
            const BYTE* p11 = r1 + (size_t)sx1 * 4;
            BYTE* q = drow + (size_t)x * 4;

            const int64_t wx0 = 65536 - fx;
            const int64_t wx1 = fx;
            const int64_t wy0 = 65536 - fy;
            const int64_t wy1 = fy;
            for (int c = 0; c < 4; c++) {
                int64_t top = (int64_t)p00[c] * wx0 + (int64_t)p01[c] * wx1;
                int64_t bot = (int64_t)p10[c] * wx0 + (int64_t)p11[c] * wx1;
                q[c] = (BYTE)((top * wy0 + bot * wy1) >> 32);
            }
        }
    }
}

// Честный fit (letterbox/pillarbox) в произвольный target: сохраняет пропорции,
// центрирует, остальное — чёрное. Fixed-point 16.16 билинейный.
inline void LetterboxBilinearEx(const BYTE* src, UINT sw, UINT sh, LONG sstride,
                                BYTE* dst, UINT tw, UINT th, LONG dstride)
{
    if (tw == 0 || th == 0) return;
    memset(dst, 0, (size_t)dstride * th);
    if (sw == 0 || sh == 0) return;
    if (sw < 2 || sh < 2) {
        LetterboxNearestEx(src, sw, sh, sstride, dst, tw, th, dstride);
        return;
    }

    double scale = (double)tw / sw;
    if ((double)th / sh < scale) scale = (double)th / sh;
    int dw = (int)llround((double)sw * scale);
    int dh = (int)llround((double)sh * scale);
    if (dw < 1) dw = 1;
    if (dh < 1) dh = 1;
    if (dw > (int)tw) dw = (int)tw;
    if (dh > (int)th) dh = (int)th;
    int x0 = ((int)tw - dw) / 2;
    int y0 = ((int)th - dh) / 2;

    std::vector<BYTE> tmp((size_t)dw * dh * 4);
    StretchBilinearEx(src, sw, sh, sstride, tmp.data(), (UINT)dw, (UINT)dh, (LONG)(dw * 4));
    for (int y = 0; y < dh; y++) {
        memcpy(dst + (size_t)(y0 + y) * dstride + (size_t)x0 * 4,
               tmp.data() + (size_t)y * dw * 4, (size_t)dw * 4);
    }
}

// Честный cover в произвольный target: масштабирует с обрезкой центра,
// пропорции сохраняет, полей нет.
inline void CoverBilinearEx(const BYTE* src, UINT sw, UINT sh, LONG sstride,
                            BYTE* dst, UINT tw, UINT th, LONG dstride)
{
    if (tw == 0 || th == 0) return;
    memset(dst, 0, (size_t)dstride * th);
    if (sw == 0 || sh == 0) return;
    if (sw < 2 || sh < 2) {
        StretchBilinearEx(src, sw, sh, sstride, dst, tw, th, dstride);
        return;
    }

    double scale = (double)tw / sw;
    if ((double)th / sh > scale) scale = (double)th / sh;
    int dw = (int)llround((double)sw * scale);
    int dh = (int)llround((double)sh * scale);
    if (dw < 1) dw = 1;
    if (dh < 1) dh = 1;
    if (dw < (int)tw) dw = (int)tw;
    if (dh < (int)th) dh = (int)th;

    std::vector<BYTE> tmp((size_t)dw * dh * 4);
    StretchBilinearEx(src, sw, sh, sstride, tmp.data(), (UINT)dw, (UINT)dh, (LONG)(dw * 4));
    int x0 = (dw - (int)tw) / 2;
    int y0 = (dh - (int)th) / 2;
    for (UINT y = 0; y < th; y++) {
        memcpy(dst + (size_t)y * dstride,
               tmp.data() + (size_t)(y0 + y) * dw * 4 + (size_t)x0 * 4,
               (size_t)tw * 4);
    }
}

// Честный fit (letterbox/pillarbox): сохраняет пропорции, центрирует,
// остальное — чёрное. Fixed-point 16.16 билинейный.
inline void LetterboxBilinear(const BYTE* src, UINT sw, UINT sh, LONG sstride,
                              BYTE* dst, LONG dstride)
{
    LetterboxBilinearEx(src, sw, sh, sstride, dst,
                        vcam::VCamWidth, vcam::VCamHeight, dstride);
}

} // namespace vcam
