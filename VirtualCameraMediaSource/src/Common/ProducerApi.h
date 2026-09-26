#pragma once

#include <cstdint>
#include <memory>
#include <string>

// Десериализованный конфиг активного источника (см. Settings — новая схема).
struct SourceConfig {
    std::wstring type;         // L"static" | L"video"
    std::wstring path;
    std::wstring scaleMode;    // L"fit" | L"cover" | L"crop" (static)
    int cropX = 0;
    int cropY = 0;
    int cropW = 0;
    int cropH = 0;
    bool cropKeepAspect = false;

    bool operator==(const SourceConfig& o) const
    {
        return type == o.type && path == o.path && scaleMode == o.scaleMode &&
               cropX == o.cropX && cropY == o.cropY && cropW == o.cropW &&
               cropH == o.cropH && cropKeepAspect == o.cropKeepAspect;
    }
    bool operator!=(const SourceConfig& o) const { return !(*this == o); }
};

// Источник кадров 1280x720 BGRX (stride >= vcam::VCamStride, верхняя строка первая).
struct IFrameSource {
    virtual bool Open(const SourceConfig& cfg, std::wstring& err) = 0;
    // false = данных сейчас нет (ещё не открыт / нет кадра / ошибка) — вызывающий
    // не должен писать такой кадр в shared memory.
    virtual bool Render(uint8_t* bgrx, int stride, std::wstring& err) = 0;
    virtual void Close() = 0;
    virtual const wchar_t* Name() const = 0;
    virtual ~IFrameSource() = default;
};

// L"static" -> StaticImageSource, L"video" -> VideoFileSource, иное -> nullptr.
std::unique_ptr<IFrameSource> CreateSource(const std::wstring& type);
