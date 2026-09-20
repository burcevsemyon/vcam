#pragma once
#include <stdint.h>

namespace vcam {

enum class VCamPixelFormat : UINT32 {
    RGB32 = 0,
    NV12 = 1
};

constexpr UINT32 VCamMagic = 0x5643414D;        // 'V','C','A','M'
constexpr UINT32 VCamVersion = 1;
constexpr UINT32 VCamWidth = 1280;
constexpr UINT32 VCamHeight = 720;
constexpr UINT32 VCamStride = VCamWidth * 4;    // 5120
constexpr UINT32 VCamPixelSize = 4;             // RGB32
constexpr UINT32 VCamFrameSize = VCamHeight * VCamStride; // 3686400
constexpr UINT32 VCamSlotCount = 8;
constexpr UINT32 VCamFrameInterval100ns = 333333;
constexpr DWORD VCamReadyTimeoutMs = 40;

struct VCamSectionHeader {
    UINT32 magic;
    UINT32 version;
    UINT32 width;
    UINT32 height;
    UINT32 stride;
    UINT32 pixelFormat;      // vcam::VCamPixelFormat (RGB32 = 0, NV12 = 1)
    UINT32 frameSize;
    UINT32 slotCount;
    UINT32 frameWriteIndex;
    volatile LONGLONG seq;   // seqlock: odd = writing, even = stable
    UINT64 lastFrameTime100ns;
    UINT64 reserved[2];
};

constexpr const wchar_t* VCamSectionName = L"Global\\VCam.FrameBuffer.v1";
constexpr const wchar_t* VCamReadyEventName = L"Global\\VCam.FrameReady.v1";
constexpr const wchar_t* VCamDacSddl =
    L"D:(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;LS)(A;;GA;;;NS)(A;;GA;;;WD)";

} // namespace vcam
