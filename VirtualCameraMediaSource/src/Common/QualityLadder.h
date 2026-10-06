#pragma once

#include <cstdint>
#include <string>

#include "SharedMemoryContract.h"

// Чистая математика лесенки качества v2 (quality source/1080p/720p).
// Вынесена из FrameWriter.h для тестируемости без MF/ATL/shm зависимостей.
// Одна функция — общий знаменатель продакшна и тестов.
namespace vcam {

// source -> fit-кламп входа к cap 4K; fixed1080p -> fit-кламп к 1920x1080
// (меньше — passthrough, лесенка только вниз); fixed720p -> 1280x720.
// false = мусор входа (0 или > 8192).
inline bool ResolveV2Size(uint32_t srcW, uint32_t srcH, const std::wstring& quality,
                          uint32_t& outW, uint32_t& outH)
{
    if (srcW == 0 || srcH == 0 || srcW > 8192 || srcH > 8192) return false;
    if (quality == L"fixed720p") {
        outW = VCamWidth;
        outH = VCamHeight;
        return true;
    }
    if (quality == L"fixed1080p") {
        constexpr uint32_t capW = 1920, capH = 1080;
        if (srcW <= capW && srcH <= capH) {
            outW = srcW;
            outH = srcH;
            return true;
        }
        double s = (double)capW / srcW;
        double s2 = (double)capH / srcH;
        if (s2 < s) s = s2;
        outW = (uint32_t)(srcW * s + 0.5);
        outH = (uint32_t)(srcH * s + 0.5);
        if (outW < 1) outW = 1;
        if (outH < 1) outH = 1;
        if (outW > capW) outW = capW;
        if (outH > capH) outH = capH;
        return true;
    }
    // source: fit-clamp к cap 4K.
    if (srcW <= VCamNativeCapW && srcH <= VCamNativeCapH) {
        outW = srcW;
        outH = srcH;
        return true;
    }
    double s = (double)VCamNativeCapW / srcW;
    double s2 = (double)VCamNativeCapH / srcH;
    if (s2 < s) s = s2;
    outW = (uint32_t)(srcW * s + 0.5);
    outH = (uint32_t)(srcH * s + 0.5);
    if (outW < 1) outW = 1;
    if (outH < 1) outH = 1;
    if (outW > VCamNativeCapW) outW = VCamNativeCapW;
    if (outH > VCamNativeCapH) outH = VCamNativeCapH;
    return true;
}

} // namespace vcam
