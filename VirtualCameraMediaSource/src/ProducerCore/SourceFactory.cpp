#include "ProducerApi.h"

#include "StaticImageSource.h"
#include "VideoFileSource.h"

std::unique_ptr<IFrameSource> CreateSource(const std::wstring& type)
{
    if (type == L"static") return std::make_unique<StaticImageSource>();
    if (type == L"video") return std::make_unique<VideoFileSource>();
    return nullptr;
}
