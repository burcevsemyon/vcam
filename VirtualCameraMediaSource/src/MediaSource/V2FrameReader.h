#pragma once
#include <windows.h>
#include <atlbase.h>
#include "SharedMemoryContract.h"
#include "MappedViewOfFilePtr.h"

// Read-only читатель v2-секции (Global\VCam.FrameBuffer.v2 -> Local\...).
// Живой эфир НЕ disturb'ится: только OpenFileMapping(FILE_MAP_READ),
// MapView(FILE_MAP_READ), без CreateFileMapping, без записи в секцию,
// без TouchReader (heartbeat остаётся на v1, как раньше).
// Писательских тестов на живом shm нет; юнит-проверки — через inline-хелперы
// ниже на своих буферах (harness в %TEMP%).
// ProducerCore НЕ линкуется (MediaSource.vcxproj), поэтому весь код здесь,
// зависимости — только Common-заголовки.

namespace vcam_v2 {

// Валидация снимка заголовка v2. mappedSize — размер замапленного региона
// (VirtualQuery), слоты фиксированного питча VCamV2MaxFrameSize (писатель:
// FrameWriter::PublishLockedV2). Мусор -> false -> фолбэк на v1.
inline bool IsV2HeaderValid(const vcam::VCamSectionHeader* h, SIZE_T mappedSize)
{
    if (h == nullptr) return false;
    if (h->magic != vcam::VCamMagic) return false;
    if (h->version != vcam::VCamVersionV2) return false;
    if (h->pixelFormat != (UINT32)vcam::VCamPixelFormat::RGB32) return false;
    if (h->width == 0 || h->width > vcam::VCamV2MaxWidth) return false;
    if (h->height == 0 || h->height > vcam::VCamV2MaxHeight) return false;
    if (h->stride < h->width * 4 || h->stride > vcam::VCamV2MaxStride) return false;
    const UINT64 need = (UINT64)h->stride * h->height;
    if (h->frameSize < need || (UINT64)h->frameSize > vcam::VCamV2MaxFrameSize) return false;
    if (h->slotCount == 0 || h->slotCount > vcam::VCamV2SlotCount) return false;
    const UINT64 total = (UINT64)sizeof(vcam::VCamSectionHeader) +
                         (UINT64)h->slotCount * vcam::VCamV2MaxFrameSize;
    if (total > (UINT64)mappedSize) return false;
    return true;
}

// Натив рекламируем отдельной RGB32-парой только если он валиден, в cap'е и
// НЕ совпадает с базовым 720p (дубликат запрещён).
inline bool ShouldAdvertiseNative(UINT32 w, UINT32 h)
{
    if (w == 0 || h == 0) return false;
    if (w > vcam::VCamV2MaxWidth || h > vcam::VCamV2MaxHeight) return false;
    if (w == vcam::VCamWidth && h == vcam::VCamHeight) return false;
    return true;
}

// Letterbox-fit даунскейл (/апскейл) RGB32 nearest-neighbour: 16:9 в 16:9 —
// fill 1:1, иное — вписывание с чёрными полями и центрированием. Та же
// математика, что MediaStream::DownsampleRgb32 (v1-путь НЕ тронут,
// бит-паритет старых путей сохраняется); dst stride — параметром.
inline void DownscaleRgb32Letterbox(const BYTE* pSrc, SIZE_T srcStride, UINT32 srcW, UINT32 srcH,
                                    BYTE* pDst, SIZE_T dstStride, UINT32 dstW, UINT32 dstH)
{
    UINT32 outW = dstW, outH = dstH;
    if ((UINT64)srcW * dstH > (UINT64)dstW * srcH) {
        outH = (UINT32)((UINT64)srcH * dstW / srcW); // вписываем по ширине
        if (outH == 0) outH = 1;
    } else {
        outW = (UINT32)((UINT64)srcW * dstH / srcH); // вписываем по высоте
        if (outW == 0) outW = 1;
    }
    const UINT32 offX = (dstW - outW) / 2;
    const UINT32 offY = (dstH - outH) / 2;

    for (UINT32 y = 0; y < dstH; ++y)
        memset(pDst + (SIZE_T)y * dstStride, 0, (SIZE_T)dstW * 4);
    for (UINT32 y = 0; y < outH; ++y) {
        const UINT32 srcY = (UINT32)((UINT64)y * srcH / outH);
        const BYTE* pSrcRow = pSrc + (SIZE_T)srcY * srcStride;
        BYTE* pDstRow = pDst + (SIZE_T)(y + offY) * dstStride + (SIZE_T)offX * 4;
        for (UINT32 x = 0; x < outW; ++x) {
            const UINT32 srcX = (UINT32)((UINT64)x * srcW / outW);
            const BYTE* pSrcPixel = pSrcRow + (SIZE_T)srcX * 4;
            BYTE* pDstPixel = pDstRow + (SIZE_T)x * 4;
            // Alpha — как старые пути (замечание Sub1: декодер 0 / MFT 255 /
            // CPU 0): копируем байт как есть, мир не чиним.
            pDstPixel[0] = pSrcPixel[0];
            pDstPixel[1] = pSrcPixel[1];
            pDstPixel[2] = pSrcPixel[2];
            pDstPixel[3] = pSrcPixel[3];
        }
    }
}

class V2Reader {
public:
    V2Reader();
    ~V2Reader();
    V2Reader(const V2Reader&) = delete;
    V2Reader& operator=(const V2Reader&) = delete;

    // Best-effort открытие read-only (Global -> Local). После успеха держит
    // открытым; при невалидном заголовке закрывает и врёт false (ретрай при
    // следующем вызове). Потокобезопасно (внутренний CS на открытие).
    bool EnsureOpen();
    bool IsOpen() const { return m_pHeader != nullptr; }

    // Снимок диметров без ожидания (seqlock-триал, без события). false — нет
    // секции / мусор / писатель внутри публикации.
    bool Probe(UINT32* pW, UINT32* pH, UINT32* pStride, UINT32* pFrameSize);

    // Кадр в pDest (cap байт): ожидание события v2 (или seq-poll без события),
    // затем seqlock-копия строк. false — всё, что не свежий валидный кадр;
    // вызывающий идёт старым v1-путём бит-в-бит.
    bool Acquire(BYTE* pDest, SIZE_T cap, UINT32* pW, UINT32* pH, UINT32* pStride,
                 DWORD timeoutMs);

private:
    void Close();
    bool ReadSnapshot(UINT32* pW, UINT32* pH, UINT32* pStride, UINT32* pFrameSize,
                      UINT32* pIdx);

    mutable ATL::CComAutoCriticalSection m_cs;
    ATL::CHandle m_hSection;
    ATL::CHandle m_hReadyEvent; // SYNCHRONIZE-only: ждём, не сбрасываем
    vcam::MappedViewOfFilePtr m_view;
    const vcam::VCamSectionHeader* m_pHeader = nullptr;
    SIZE_T m_cbMapped = 0;
};

} // namespace vcam_v2
