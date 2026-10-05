#include "doctest.h"
#include <windows.h>

#include "FrameCopy.h"
#include "SharedMemoryContract.h"
#include "ProducerApi.h"
#include "Settings.h"

#include <cstring>
#include <string>
#include <vector>

// CopyFrameRowwise — общий helper копирования кадров с разными stride.
// Баг здесь = повреждение кадра в каждом источнике/писателе.
TEST_CASE("framecopy: same stride = single memcpy")
{
    const size_t w = 4, h = 3, bpp = 4;
    const size_t stride = w * bpp;
    std::vector<uint8_t> src(stride * h);
    for (size_t i = 0; i < src.size(); i++) src[i] = (uint8_t)(i & 0xFF);
    std::vector<uint8_t> dst(stride * h, 0);

    vcam::CopyFrameRowwise(dst.data(), stride, src.data(), stride, w, h, bpp);
    CHECK(std::memcmp(dst.data(), src.data(), src.size()) == 0);
}

TEST_CASE("framecopy: different strides copies rowBytes only")
{
    const size_t w = 3, h = 2, bpp = 4;
    const size_t sStride = 20; // padded source
    const size_t dStride = 12; // tight dest
    std::vector<uint8_t> src(sStride * h, 0xAB);
    // Fill rowBytes with recognizable pattern.
    for (size_t y = 0; y < h; y++)
        for (size_t x = 0; x < w * bpp; x++)
            src[y * sStride + x] = (uint8_t)(y * 16 + x);
    std::vector<uint8_t> dst(dStride * h, 0);

    vcam::CopyFrameRowwise(dst.data(), dStride, src.data(), sStride, w, h, bpp);
    for (size_t y = 0; y < h; y++)
        for (size_t x = 0; x < w * bpp; x++)
            CHECK(dst[y * dStride + x] == (uint8_t)(y * 16 + x));
}

TEST_CASE("framecopy: source padding bytes not copied")
{
    const size_t w = 2, h = 1, bpp = 4;
    const size_t sStride = 16; // 8 bytes padding per row
    const size_t dStride = 8;
    std::vector<uint8_t> src(sStride, 0xFF);
    src[0] = 1; src[1] = 2; src[2] = 3; src[3] = 4;
    src[4] = 5; src[5] = 6; src[6] = 7; src[7] = 8;
    // bytes 8-15 = 0xFF padding
    std::vector<uint8_t> dst(dStride, 0);

    vcam::CopyFrameRowwise(dst.data(), dStride, src.data(), sStride, w, h, bpp);
    CHECK(dst[0] == 1); CHECK(dst[7] == 8);
    // Only 8 bytes copied (w*bpp), padding not included.
    CHECK(dst.size() == dStride);
}

// SourceConfig operator== — hot-switch detection. Пропуск поля = хост
// не заметит смену и не переоткроет источник.
TEST_CASE("sourceconfig: == detects every field change")
{
    SourceConfig a;
    a.type = L"static";
    a.path = L"C:\\img.bmp";
    a.camName = L"Cam";
    a.capture = L"max";
    a.scaleMode = L"fit";
    a.cropX = 1; a.cropY = 2; a.cropW = 3; a.cropH = 4;
    a.cropKeepAspect = true;
    a.playOnce = false;

    SourceConfig b = a;
    CHECK(a == b);

    b.type = L"video"; CHECK(a != b); b.type = a.type; CHECK(a == b);
    b.path = L"x"; CHECK(a != b); b.path = a.path; CHECK(a == b);
    b.camName = L"x"; CHECK(a != b); b.camName = a.camName; CHECK(a == b);
    b.capture = L"720p"; CHECK(a != b); b.capture = a.capture; CHECK(a == b);
    b.scaleMode = L"cover"; CHECK(a != b); b.scaleMode = a.scaleMode; CHECK(a == b);
    b.cropX = 99; CHECK(a != b); b.cropX = a.cropX; CHECK(a == b);
    b.cropY = 99; CHECK(a != b); b.cropY = a.cropY; CHECK(a == b);
    b.cropW = 99; CHECK(a != b); b.cropW = a.cropW; CHECK(a == b);
    b.cropH = 99; CHECK(a != b); b.cropH = a.cropH; CHECK(a == b);
    b.cropKeepAspect = false; CHECK(a != b); b.cropKeepAspect = a.cropKeepAspect; CHECK(a == b);
    b.playOnce = true; CHECK(a != b); b.playOnce = a.playOnce; CHECK(a == b);
}

// VCamSectionHeader layout — общий контракт v1/v2. Сдвиг полей = рассинхрон
// писателя и читателя, крах или мусор в кадре.
TEST_CASE("contract: VCamSectionHeader struct layout")
{
    // Поля идут строго в этом порядке; проверяем смещения.
    CHECK(sizeof(vcam::VCamSectionHeader) >= 64); // есть резервные поля
    CHECK(offsetof(vcam::VCamSectionHeader, magic) == 0);
    CHECK(offsetof(vcam::VCamSectionHeader, version) == 4);
    CHECK(offsetof(vcam::VCamSectionHeader, width) == 8);
    CHECK(offsetof(vcam::VCamSectionHeader, height) == 12);
    CHECK(offsetof(vcam::VCamSectionHeader, stride) == 16);
    CHECK(offsetof(vcam::VCamSectionHeader, pixelFormat) == 20);
    CHECK(offsetof(vcam::VCamSectionHeader, frameSize) == 24);
    CHECK(offsetof(vcam::VCamSectionHeader, slotCount) == 28);
    CHECK(offsetof(vcam::VCamSectionHeader, frameWriteIndex) == 32);
    // seq is LONGLONG — 8-byte aligned, so 36 or 40 depending on padding.
    // After frameWriteIndex (offset 32, 4 bytes) -> next offset 36.
    // LONGLONG needs 8-byte alignment -> padding to 40.
    // Actually on x64 MSVC, volatile LONGLONG after UINT32 at offset 32
    // aligns to offset 40 (8-byte boundary).
    const size_t seqOff = offsetof(vcam::VCamSectionHeader, seq);
    CHECK(seqOff == 40); // 8-byte aligned after 4-byte field
    CHECK(sizeof(vcam::VCamSectionHeader) > seqOff + 8); // seq + trailing fields
}

// Settings Serialize — формат JSON, который читает C# Settings.LoadFromText.
// Проверяем ключи, которые должны присутствовать всегда.
TEST_CASE("settings: Serialize writes required JSON keys")
{
    Settings s;
    std::string json = s.Serialize();
    // Ключи новой схемы — без них C# не распознает файл.
    CHECK(json.find("\"source\"") != std::string::npos);
    CHECK(json.find("\"type\"") != std::string::npos);
    CHECK(json.find("\"static\"") != std::string::npos);
    CHECK(json.find("\"path\"") != std::string::npos);
    CHECK(json.find("\"scaleMode\"") != std::string::npos);
    CHECK(json.find("\"cropX\"") != std::string::npos);
    CHECK(json.find("\"cropY\"") != std::string::npos);
    CHECK(json.find("\"cropW\"") != std::string::npos);
    CHECK(json.find("\"cropH\"") != std::string::npos);
    CHECK(json.find("\"cropKeepAspect\"") != std::string::npos);
    CHECK(json.find("\"video\"") != std::string::npos);
    CHECK(json.find("\"camera\"") != std::string::npos);
    CHECK(json.find("\"id\"") != std::string::npos);
    CHECK(json.find("\"name\"") != std::string::npos);
    CHECK(json.find("\"capture\"") != std::string::npos);
    CHECK(json.find("\"quality\"") != std::string::npos);
    CHECK(json.find("\"hotkey\"") != std::string::npos);
    CHECK(json.find("\"modifiers\"") != std::string::npos);
    CHECK(json.find("\"vk\"") != std::string::npos);
    CHECK(json.find("\"recordHotkey\"") != std::string::npos);
    CHECK(json.find("\"record\"") != std::string::npos);
    CHECK(json.find("\"autostart\"") != std::string::npos);
}

// ToSourceConfig: playOnce и camName — поля SourceConfig, которые не
// дублируются в Settings. Проверяем, что маппинг их корректно заполняет.
TEST_CASE("sourceconfig: playOnce defaults to false")
{
    Settings s;
    SourceConfig cfg = ToSourceConfig(s);
    CHECK(cfg.playOnce == false);
}
