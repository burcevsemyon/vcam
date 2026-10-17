#pragma once

#include <cstdint>
#include <cstring>

// Общий letterbox-fit даунскейл/апскейл RGB32 nearest-neighbour.
// Вынесен из MediaStream.cpp (DownsampleRgb32) и V2FrameReader.h
// (DownscaleRgb32Letterbox) — общий код для продакшна и тестов.
// Единый вариант с zero-guard (безопаснее исходных копий).
namespace vcam {

// Вписывает src в dst с сохранением пропорций (letterbox/pillarbox),
// центрирует и заливает поля чёрным. Nearest-neighbour sampling.
// srcStride / dstStride — в байтах. Оба варианта (с stride=dstW*4 и
// с произвольным stride) покрываются одним вызовом.
inline void LetterboxNearestFit(const BYTE* pSrc, uint32_t srcStride,
                                uint32_t srcW, uint32_t srcH,
                                BYTE* pDst, uint32_t dstStride,
                                uint32_t dstW, uint32_t dstH)
{
    if (dstW == 0 || dstH == 0) return;
    // Заливка чёрным (letterbox-поля).
    for (uint32_t y = 0; y < dstH; ++y)
        memset(pDst + (size_t)y * dstStride, 0, (size_t)dstW * 4);
    if (srcW == 0 || srcH == 0) return;

    // Вычисляем размер вписанного прямоугольника.
    uint32_t outW = dstW, outH = dstH;
    if ((uint64_t)srcW * dstH > (uint64_t)dstW * srcH) {
        outH = (uint32_t)((uint64_t)srcH * dstW / srcW);
        if (outH == 0) outH = 1;
    } else {
        outW = (uint32_t)((uint64_t)srcW * dstH / srcH);
        if (outW == 0) outW = 1;
    }
    if (outW > dstW) outW = dstW;
    if (outH > dstH) outH = dstH;

    const uint32_t offX = (dstW - outW) / 2;
    const uint32_t offY = (dstH - outH) / 2;

    // Nearest-neighbor sampling.
    for (uint32_t y = 0; y < outH; ++y) {
        const uint32_t srcY = (uint32_t)((uint64_t)y * srcH / outH);
        const BYTE* pSrcRow = pSrc + (size_t)srcY * srcStride;
        BYTE* pDstRow = pDst + (size_t)(y + offY) * dstStride + (size_t)offX * 4;
        for (uint32_t x = 0; x < outW; ++x) {
            const uint32_t srcX = (uint32_t)((uint64_t)x * srcW / outW);
            const BYTE* pSrcPixel = pSrcRow + (size_t)srcX * 4;
            BYTE* pDstPixel = pDstRow + (size_t)x * 4;
            pDstPixel[0] = pSrcPixel[0];
            pDstPixel[1] = pSrcPixel[1];
            pDstPixel[2] = pSrcPixel[2];
            pDstPixel[3] = pSrcPixel[3];
        }
    }
}

} // namespace vcam
