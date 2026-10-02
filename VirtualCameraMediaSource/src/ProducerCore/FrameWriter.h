#pragma once

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

#include "SharedMemoryContract.h"
#include "MappedViewOfFilePtr.h"

// Единственный писатель в shared memory (Global\VCam.FrameBuffer.v1).
// Публикация: seqlock, slot = (idx+1) % 8, 30 FPS pacing внутри WriteFrame/FlushLast.
// Если создать Global\ невозможно (Limited-токен без SeCreateGlobalPrivilege) —
// открываем существующую секцию либо создаём сеансовую Local\-копию (читатели
// в SharedMemoryFrameSource перебирают префиксы Global\ -> Local\ сами).
class FrameWriter {
public:
    FrameWriter();
    ~FrameWriter();

    FrameWriter(const FrameWriter&) = delete;
    FrameWriter& operator=(const FrameWriter&) = delete;

    bool Open(std::wstring& err);
    void Close();
    bool IsOpen() const { return open_; }
    // Какое имя секции реально использовалось (для лога хоста).
    const std::wstring& SectionOpenedAs() const { return openedSection_; }

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
    vcam::MappedViewOfFilePtr view_;
    vcam::VCamSectionHeader* pHeader_ = nullptr;
    std::vector<uint8_t> cache_;
    bool hasFrame_ = false;
    bool open_ = false;
    std::wstring openedSection_;
    LARGE_INTEGER freq_ = {};
    LONGLONG startCount_ = 0;
    LONGLONG lastEmit_ = 0;
};
