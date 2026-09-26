#pragma once

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

#include "ProducerApi.h"

// Статическая картинка: при Open грузится сразу в готовый буфер 1280x720 BGRX,
// Render только отдаёт его. Повторный Open с тем же конфигом ничего не делает.
class StaticImageSource : public IFrameSource {
public:
    StaticImageSource() = default;
    ~StaticImageSource() override { Close(); }

    bool Open(const SourceConfig& cfg, std::wstring& err) override;
    bool Render(uint8_t* bgrx, int stride, std::wstring& err) override;
    void Close() override;
    const wchar_t* Name() const override { return L"static"; }

private:
    SourceConfig cfg_;
    bool open_ = false;
    std::vector<uint8_t> frame_;
};
