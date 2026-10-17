#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

// Общий helper паттерна «равные stride → одним memcpy, иначе построчно»
// (раньше дублировался в VideoFileSource/StaticImageSource/CameraSource/
// FrameWriter/SharedMemoryFrameSource). rowBytes = width * bpp; ветка
// одного memcpy срабатывает только когда строки непрерывны (rowBytes == stride)
// — результат байт-в-байт совпадает с построчным копированием.

namespace vcam {

inline void CopyFrameRowwise(uint8_t* dst, size_t dStride, const uint8_t* src,
                             size_t sStride, size_t width, size_t height,
                             size_t bpp)
{
    const size_t rowBytes = width * bpp;
    if (dStride == sStride && rowBytes == dStride) {
        memcpy(dst, src, rowBytes * height);
        return;
    }
    for (size_t y = 0; y < height; y++) {
        memcpy(dst + y * dStride, src + y * sStride, rowBytes);
    }
}

} // namespace vcam
