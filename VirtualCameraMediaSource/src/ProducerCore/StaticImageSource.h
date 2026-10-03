#pragma once

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

#include "ProducerApi.h"

// Статическая картинка: при Open грузится готовый буфер 1280x720 BGRX (WIC,
// legacy v1-путь — бит-в-бит как раньше) + нативный декод (cap 4K) для v2.
// Render только отдаёт кэш; RenderSized масштабирует натив CPU-helpers.
// Повторный Open с тем же конфигом ничего не делает.
class StaticImageSource : public IFrameSource {
public:
    StaticImageSource() = default;
    ~StaticImageSource() override { Close(); }

    bool Open(const SourceConfig& cfg, std::wstring& err) override;
    bool Render(uint8_t* bgrx, int stride, std::wstring& err) override;
    bool Render(uint8_t* dst, int stride, uint32_t w, uint32_t h,
                std::wstring& err) override;
    bool NativeSize(uint32_t& w, uint32_t& h) override;
    void Close() override;
    const wchar_t* Name() const override { return L"static"; }

private:
    SourceConfig cfg_;
    bool open_ = false;
    std::vector<uint8_t> frame_; // 1280x720 BGRX (WIC legacy)
    std::vector<uint8_t> native_; // натив BGRX, stride nativeW_*4, top-down
    uint32_t nativeW_ = 0;
    uint32_t nativeH_ = 0;
    double cropScaleX_ = 1.0; // nativeW_/исходная ширина (crop-rect в исх. пикселях)
    double cropScaleY_ = 1.0;
};
