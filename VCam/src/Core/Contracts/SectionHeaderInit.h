#pragma once

#include "SharedMemoryContract.h"

// Общий init полей VCamSectionHeader (сверено байт-в-байт: FrameWriter::Open и
// SharedMemoryFrameSource::Init писали одинаковый набор 12 полей; условия
// вызова — безусловный init писателя / if (magic != VCamMagic) читателя —
// остаются у вызывающих. Seqlock publish/read-циклы не затронуты.

namespace vcam {

inline void InitSectionHeader(VCamSectionHeader* h)
{
    h->magic = VCamMagic;
    h->version = VCamVersion;
    h->width = VCamWidth;
    h->height = VCamHeight;
    h->stride = VCamStride;
    h->pixelFormat = (UINT32)VCamPixelFormat::RGB32;
    h->frameSize = VCamFrameSize;
    h->slotCount = VCamSlotCount;
    h->frameWriteIndex = 0;
    h->seq = 0;
    h->lastFrameTime100ns = 0;
    h->readerLastActiveTick = 0;
}

// v2 (ADD): version=2, слотов 4, диметры текущего кадра (писатель обновляет их
// внутри seqlock при каждой публикации — см. контракт v2).
inline void InitSectionHeaderV2(VCamSectionHeader* h, UINT32 w, UINT32 hgt, UINT32 stride)
{
    h->magic = VCamMagic;
    h->version = VCamVersionV2;
    h->width = w;
    h->height = hgt;
    h->stride = stride;
    h->pixelFormat = (UINT32)VCamPixelFormat::RGB32;
    h->frameSize = hgt * stride;
    h->slotCount = VCamV2SlotCount;
    h->frameWriteIndex = 0;
    h->seq = 0;
    h->lastFrameTime100ns = 0;
    h->readerLastActiveTick = 0;
}

} // namespace vcam
