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
    // Heartbeat активного потребителя (MediaSource, GetTickCount64): пишется при
    // каждой доставке сэмплов живой MF-сессии. Хост на «Выход» и hold-watch
    // холдер используют для решения «потребитель есть / нет». Лейаут и версия v1
    // не менялись (бывший reserved[0]); старые читатели пишут 0 = «нет».
    UINT64 readerLastActiveTick;
    UINT64 reserved;
};

constexpr const wchar_t* VCamSectionName = L"Global\\VCam.FrameBuffer.v1";
constexpr const wchar_t* VCamReadyEventName = L"Global\\VCam.FrameReady.v1";
constexpr const wchar_t* VCamDacSddl =
    L"D:(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;LS)(A;;GA;;;NS)(A;;GA;;;WD)";

// ---- v2 (ADD, фаза vcam-quality-v2/sub1; блок v1 выше — frozen, ни байтом) ----
// Новая секция нативного кадра. Продьюсер пишет ОБЕ: v1 (720p как раньше) +
// v2 (натив источника до cap). Старые читатели v2 не видят.
// Слоты фиксированного max-размера (cap 4K); валидные байты кадра — первые
// header.frameSize байт слота при текущих header.width/height/stride.
// Диметры меняются только внутри seqlock-публикации писателя (seq нечётный):
// читатель обязан читать seq1 -> dims+slot -> seq2 и повторять при
// несовпадении/нечётности (протокол читателя — Sub2).
constexpr UINT32 VCamVersionV2 = 2;
constexpr UINT32 VCamV2MaxWidth = 3840;
constexpr UINT32 VCamV2MaxHeight = 2160;
constexpr UINT32 VCamV2MaxStride = VCamV2MaxWidth * 4; // 15360
constexpr UINT64 VCamV2MaxFrameSize = (UINT64)VCamV2MaxHeight * VCamV2MaxStride; // 33177600
constexpr UINT32 VCamV2SlotCount = 4; // 8x33МБ жирно — 4 (итого ~126.6 МиБ)
constexpr UINT64 VCamV2TotalSize =
    (UINT64)sizeof(VCamSectionHeader) + (UINT64)VCamV2SlotCount * VCamV2MaxFrameSize;
constexpr const wchar_t* VCamSectionNameV2 = L"Global\\VCam.FrameBuffer.v2";
constexpr const wchar_t* VCamReadyEventNameV2 = L"Global\\VCam.FrameReady.v2";
// Cap натива источника (рендер/писатель клампят fit'ом; константа, UI нет).
constexpr UINT32 VCamNativeCapW = VCamV2MaxWidth;
constexpr UINT32 VCamNativeCapH = VCamV2MaxHeight;
static_assert(VCamV2MaxFrameSize == 33177600ULL, "v2 max frame math");
static_assert(VCamV2SlotCount == 4, "v2 slot count");
static_assert(sizeof(VCamSectionHeader) == 72, "v2 header layout");

} // namespace vcam
