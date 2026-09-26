#pragma once

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

#include "SharedMemoryContract.h"

// Единственный писатель в shared memory (Global\VCam.FrameBuffer.v1).
// Публикация: seqlock, slot = (idx+1) % 8, 30 FPS pacing внутри WriteFrame/FlushLast.
class FrameWriter {
public:
    FrameWriter();
    ~FrameWriter();

    FrameWriter(const FrameWriter&) = delete;
    FrameWriter& operator=(const FrameWriter&) = delete;

    bool Open(std::wstring& err);
    void Close();
    bool IsOpen() const { return open_; }

    // Кладёт кадр в shared memory и кэширует его как последний удачный.
    bool WriteFrame(const uint8_t* bgrx, int stride);
    // Повторно публикует последний удачный кадр (hot-switch, пока новый источник
    // не дал первый кадр). false — кадра ещё не было / writer не открыт.
    bool FlushLast();

private:
    bool PublishLocked();
    void Pace();

    CRITICAL_SECTION cs_ = {};
    bool csInit_ = false;
    HANDLE hSection_ = nullptr;
    HANDLE hReady_ = nullptr;
    PSECURITY_DESCRIPTOR pSecDesc_ = nullptr;
    uint8_t* pBase_ = nullptr;
    vcam::VCamSectionHeader* pHeader_ = nullptr;
    std::vector<uint8_t> cache_;
    bool hasFrame_ = false;
    bool open_ = false;
    LARGE_INTEGER freq_ = {};
    LONGLONG startCount_ = 0;
    LONGLONG lastEmit_ = 0;
};
