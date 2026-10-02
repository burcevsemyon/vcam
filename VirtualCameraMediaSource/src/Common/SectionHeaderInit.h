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

} // namespace vcam
