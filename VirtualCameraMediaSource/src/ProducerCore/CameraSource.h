#pragma once

#include <windows.h>
#include <atlbase.h>

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "ProducerApi.h"
#include "VideoProcessorScaler.h"

struct IMFSourceReader;
struct IMFMediaSource;

// Захват с физической камеры через MF SourceReader. Фоновый поток читает
// сэмплы (RGB32 capW x capH) в кэш последнего кадра под мьютексом; Render
// отдаёт кэш в буфер хоста (построчный memcpy при 1280x720, иначе сначала
// Video Processor MFT, при его недоступности — letterbox-fit билинейный). Ошибка/молчание камеры ~3 с -> failed_ и Render=false с причиной
// (хост уходит в fallback NO SIGNAL; никакого keep-previous).
class CameraSource : public IFrameSource {
public:
    CameraSource();
    ~CameraSource() override;

    CameraSource(const CameraSource&) = delete;
    CameraSource& operator=(const CameraSource&) = delete;

    bool Open(const SourceConfig& cfg, std::wstring& err) override;
    bool Render(uint8_t* bgrx, int stride, std::wstring& err) override;
    void Close() override;
    const wchar_t* Name() const override { return L"camera"; }

private:
    static DWORD WINAPI ThreadProc(LPVOID self);
    void CaptureLoop();
    void SetFailed(const std::wstring& reason);
    // Остановка capture-потока с таймаутом: false = поток не успел выйти,
    // reader/MF/состояние не тронуты (иначе — UAF под живым CaptureLoop).
    bool Shutdown(DWORD timeoutMs);

    std::mutex mutex_;           // кэш + флаги: поток захвата <-> Render
    SourceConfig cfg_;
    ATL::CComPtr<IMFSourceReader> reader_;  // создатель/владелец — Open/Close (поток хоста)
    ATL::CComPtr<IMFMediaSource> mediaSrc_; // ActivateObject; Shutdown+Release в Close
    ATL::CHandle thread_;
    ATL::CHandle stopEvent_;
    DWORD streamIndex_ = 0;
    std::vector<uint8_t> cache_; // RGB32, capW*4 stride, верхняя строка первая
    UINT32 capW_ = 0;
    UINT32 capH_ = 0;
    LONG capStride_ = 0;         // stride источника (может быть отрицательным)
    bool open_ = false;
    bool hasFrame_ = false;
    bool failed_ = false;
    bool mfUp_ = false;          // парный MFStartup/MFShutdown
    bool comUp_ = false;         // парный CoInitializeEx/CoUninitialize
    std::wstring failReason_;
    VideoProcessorScaler mftScaler_; // GPU-скейл кэша; недоступен -> CPU-fallback
};
