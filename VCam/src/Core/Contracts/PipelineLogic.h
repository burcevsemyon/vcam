#pragma once

#include <cstdint>
#include <string>

#include "SharedMemoryContract.h"

// Чистая логика PipelineEngine и FrameWriter без MF/shm зависимостей.
// Вынесена для тестируемости. При изменении PipelineEngine.cpp /
// FrameWriter.h — синхронизировать!
namespace vcam {

// Нормализация токена quality: только source|fixed1080p|fixed720p,
// остальное -> source. Используется в PipelineEngine::SetTarget и
// FrameWriter::SetQuality (одна логика, два вызывающих).
inline std::wstring NormalizeQuality(const std::wstring& q)
{
    if (q == L"fixed720p") return L"fixed720p";
    if (q == L"fixed1080p") return L"fixed1080p";
    return L"source";
}

// Валидация размеров кадра для буфера PipelineEngine (EnsureFrameBuf).
// false = недопустимые размеры (0, > cap 4K, > max frame size).
inline bool ValidateFrameDims(uint32_t w, uint32_t h)
{
    if (w == 0 || h == 0) return false;
    if (w > VCamNativeCapW || h > VCamNativeCapH) return false;
    const uint64_t need = (uint64_t)h * w * 4;
    if (need == 0 || need > VCamV2MaxFrameSize) return false;
    return true;
}

// Размер буфера в байтах для кадра WxH BGRX.
inline uint64_t CalcFrameBytes(uint32_t w, uint32_t h)
{
    return (uint64_t)h * w * 4;
}

// Решение RenderOne: использовать нативный размер или fallback на 720p.
// true = натив валиден и в пределах cap; false = fallback 720p.
// Логика: NativeSize OK + dims ненулевые + в пределах cap.
inline bool ShouldUseNative(uint32_t nativeW, uint32_t nativeH)
{
    return nativeW != 0 && nativeH != 0 &&
           nativeW <= VCamNativeCapW && nativeH <= VCamNativeCapH;
}

// Нормализация quality для FrameWriter::SetQuality (дублирует
// NormalizeQuality, но оставлен как отдельная точка — FrameWriter
// не включает PipelineLogic.h из-за цепочки зависимостей).
inline std::wstring NormalizeQualityWriter(const std::wstring& q)
{
    return NormalizeQuality(q);
}

} // namespace vcam
