#include "doctest.h"
#include <windows.h>

#include "../MediaSource/V2FrameReader.h"

#include <cstdint>

// Политика (07.10.2026, ktalk-инцидент): FrameServer-прокси нестабилен на
// native RGB32 типах — 1920x1080 мерцал ~2-3 Гц, 1920x1072 не выводился
// вообще, при всегда ярком shared-буфере. Лесенка msrc = 3 типа
// (RGB720/NV12720/RGB640), натив никогда не рекламируется.
// Тест — замок на ShouldAdvertiseNative: любое true здесь = регресс.
TEST_CASE("native-advertise: never advertised (proxy stability policy)")
{
    CHECK(vcam_v2::ShouldAdvertiseNative(1920, 1080) == false);
    CHECK(vcam_v2::ShouldAdvertiseNative(1920, 1072) == false);
    CHECK(vcam_v2::ShouldAdvertiseNative(1280, 720) == false);
    CHECK(vcam_v2::ShouldAdvertiseNative(640, 480) == false);
    CHECK(vcam_v2::ShouldAdvertiseNative(3840, 2160) == false);
    CHECK(vcam_v2::ShouldAdvertiseNative(2752, 1536) == false);
    CHECK(vcam_v2::ShouldAdvertiseNative(0, 0) == false);
    CHECK(vcam_v2::ShouldAdvertiseNative(1920, 0) == false);
}
