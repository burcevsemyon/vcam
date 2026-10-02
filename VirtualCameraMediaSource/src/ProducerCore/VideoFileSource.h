#pragma once

#include <windows.h>
#include <atlbase.h>

#include <cstdint>
#include <string>
#include <vector>

#include "ProducerApi.h"

// Видеофайл через MF SourceReader: декод в фоновом потоке с frame-holding по
// timestamp и лупом SetPosition(0). Render отдаёт последний декодированный кадр;
// при ошибке открытия/декодирования возвращает false с причиной (без «продолжаем
// прошлый клип»).
class VideoFileSource : public IFrameSource {
public:
    VideoFileSource();
    ~VideoFileSource() override;

    VideoFileSource(const VideoFileSource&) = delete;
    VideoFileSource& operator=(const VideoFileSource&) = delete;

    bool Open(const SourceConfig& cfg, std::wstring& err) override;
    bool Render(uint8_t* bgrx, int stride, std::wstring& err) override;
    void Close() override;
    const wchar_t* Name() const override { return L"video"; }

private:
    static DWORD WINAPI ThreadProc(LPVOID self);
    void DecodeLoop();
    void SetFailed(const std::wstring& reason);
    // Остановка decode-потока с таймаутом: false = поток не успел выйти,
    // никакие ресурсы/состояние не тронуты (иначе — UAF под живым DecodeLoop).
    bool Shutdown(DWORD timeoutMs);

    CRITICAL_SECTION cs_ = {};
    SourceConfig cfg_;
    ATL::CHandle thread_;
    ATL::CHandle stopEvent_;
    std::vector<uint8_t> frame_;
    bool open_ = false;
    bool frameReady_ = false;
    bool failed_ = false;
    std::wstring failReason_;
};
