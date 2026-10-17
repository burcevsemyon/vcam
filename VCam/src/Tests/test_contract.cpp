#include "doctest.h"
#include <string>
#include <windows.h>
#include "SharedMemoryContract.h"

// Регрессия: VCamFrameSize = 6 553 600 (width*stride) вместо 3 686 400
// (height*stride) — FallbackFrame memset переполнял буфер → AV → крах
// FrameServer. Одна строка в SharedMemoryContract.h, но проверять надо явно.
TEST_CASE("contract: v1 frame size = height * stride (NOT width * stride)")
{
    CHECK(vcam::VCamFrameSize == 3686400u);
    CHECK(vcam::VCamFrameSize == vcam::VCamHeight * vcam::VCamStride);
    CHECK(vcam::VCamFrameSize != vcam::VCamWidth * vcam::VCamStride); // historical bug
}

TEST_CASE("contract: v1 geometry constants")
{
    CHECK(vcam::VCamWidth == 1280u);
    CHECK(vcam::VCamHeight == 720u);
    CHECK(vcam::VCamStride == 5120u);
    CHECK(vcam::VCamPixelSize == 4u);
    CHECK(vcam::VCamSlotCount == 8u);
}

TEST_CASE("contract: v2 max frame size math")
{
    CHECK(vcam::VCamV2MaxWidth == 3840u);
    CHECK(vcam::VCamV2MaxHeight == 2160u);
    CHECK(vcam::VCamV2MaxStride == 15360u);
    CHECK(vcam::VCamV2MaxFrameSize == 33177600ULL);
    CHECK(vcam::VCamV2MaxFrameSize ==
          (UINT64)vcam::VCamV2MaxHeight * vcam::VCamV2MaxStride);
    CHECK(vcam::VCamV2SlotCount == 4u);
}

TEST_CASE("contract: v2 native cap == v2 max dims")
{
    CHECK(vcam::VCamNativeCapW == vcam::VCamV2MaxWidth);
    CHECK(vcam::VCamNativeCapH == vcam::VCamV2MaxHeight);
}

TEST_CASE("contract: section names are distinct v1/v2")
{
    CHECK(std::wstring(vcam::VCamSectionName) != std::wstring(vcam::VCamSectionNameV2));
    CHECK(std::wstring(vcam::VCamReadyEventName) != std::wstring(vcam::VCamReadyEventNameV2));
}
