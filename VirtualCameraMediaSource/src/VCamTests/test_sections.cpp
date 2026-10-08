#include "doctest.h"
#include <windows.h>

#include "Settings.h"
#include "SharedMemoryContract.h"

// Операторы сравнения секций — hot-switch detection. Если == пропустит
// поле, хост не заметит смену и не переоткроет источник/не перерегистрирует
// хоткей. Каждый проверяем.

TEST_CASE("sections: StaticSection == detects all fields")
{
    StaticSection a;
    a.path = L"C:\\img.bmp";
    a.scaleMode = L"fit";
    a.cropX = 1; a.cropY = 2; a.cropW = 3; a.cropH = 4;
    a.cropKeepAspect = true;

    StaticSection b = a;
    CHECK(a == b);
    b.path = L"x"; CHECK(a != b); b.path = a.path; CHECK(a == b);
    b.scaleMode = L"cover"; CHECK(a != b); b.scaleMode = a.scaleMode; CHECK(a == b);
    b.cropX = 99; CHECK(a != b); b.cropX = a.cropX; CHECK(a == b);
    b.cropY = 99; CHECK(a != b); b.cropY = a.cropY; CHECK(a == b);
    b.cropW = 99; CHECK(a != b); b.cropW = a.cropW; CHECK(a == b);
    b.cropH = 99; CHECK(a != b); b.cropH = a.cropH; CHECK(a == b);
    b.cropKeepAspect = false; CHECK(a != b); b.cropKeepAspect = a.cropKeepAspect; CHECK(a == b);
}

TEST_CASE("sections: VideoSection detects path and loop change")
{
    VideoSection a;
    a.path = L"C:\\vid.mp4";
    a.loop = false;
    VideoSection b = a;
    CHECK(a == b);
    b.path = L"x"; CHECK(a != b); b.path = a.path; CHECK(a == b);
    b.loop = true; CHECK(a != b); // смена повтора обязана пересоздать источник
    b.loop = false; CHECK(a == b);
}

TEST_CASE("sections: CameraSection == detects all fields")
{
    CameraSection a;
    a.id = L"\\\\?\\usb#1234";
    a.name = L"Cam";
    a.capture = L"720p";

    CameraSection b = a;
    CHECK(a == b);
    b.id = L"x"; CHECK(a != b); b.id = a.id; CHECK(a == b);
    b.name = L"x"; CHECK(a != b); b.name = a.name; CHECK(a == b);
    b.capture = L"1080p"; CHECK(a != b); b.capture = a.capture; CHECK(a == b);
}

TEST_CASE("sections: HotkeySection == and !=")
{
    HotkeySection a;
    a.modifiers = 3;
    a.vk = 0x56;
    HotkeySection b = a;
    CHECK(a == b);
    CHECK(!(a != b));
    b.modifiers = 6;
    CHECK(a != b);
    CHECK(!(a == b));
    b.modifiers = 3;
    b.vk = 0x41;
    CHECK(a != b);
}

TEST_CASE("sections: RecordHotkeySection == and !=")
{
    RecordHotkeySection a;
    a.modifiers = 3;
    a.vk = 0x52;
    RecordHotkeySection b = a;
    CHECK(a == b);
    b.vk = 0x41;
    CHECK(a != b);
}

TEST_CASE("sections: VideoHotkeySection == and !=")
{
    VideoHotkeySection a;
    a.modifiers = 3;
    a.vk = 0x50;
    VideoHotkeySection b = a;
    CHECK(a == b);
    CHECK(!(a != b));
    b.modifiers = 6;
    CHECK(a != b);
    b.modifiers = 3;
    b.vk = 0x41;
    CHECK(a != b);
}

TEST_CASE("sections: SourceSwitchHotkeySection == and !=")
{
    SourceSwitchHotkeySection a;
    a.modifiers = 3;
    a.vk = 0x31;
    SourceSwitchHotkeySection b = a;
    CHECK(a == b);
    CHECK(!(a != b));
    b.modifiers = 6;
    CHECK(a != b);
    b.modifiers = 3;
    b.vk = 0x41;
    CHECK(a != b);
}

TEST_CASE("sections: Settings == detects quality change")
{
    Settings a, b;
    CHECK(a == b);
    b.quality = L"fixed720p";
    CHECK(a != b);
    b.quality = L"source";
    CHECK(a == b);
    b.quality = L"fixed1080p";
    CHECK(a != b);
}

// VCamPixelFormat enum — контракт писателя/читателя.
TEST_CASE("contract: VCamPixelFormat values")
{
    CHECK((int)vcam::VCamPixelFormat::RGB32 == 0);
    CHECK((int)vcam::VCamPixelFormat::NV12 == 1);
}

// Проверка static_assert в SharedMemoryContract.h — если структура изменит
// размер, компиляция упадёт. Здесь дублируем для явности.
TEST_CASE("contract: v2 total size math")
{
    // 4 слота × 33 177 600 + sizeof(header) ≈ 126.6 МиБ.
    const UINT64 slotsSize = (UINT64)vcam::VCamV2SlotCount * vcam::VCamV2MaxFrameSize;
    CHECK(slotsSize == 132710400ULL);
    CHECK(vcam::VCamV2TotalSize ==
          (UINT64)sizeof(vcam::VCamSectionHeader) + slotsSize);
}
