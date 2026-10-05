#pragma once

#include <windows.h>
#include <atlbase.h>

#include <cstdint>
#include <string>
#include <vector>

#include "ProducerApi.h"
#include "VideoProcessorScaler.h"

// Видеофайл через MF SourceReader: декод в фоновом потоке с frame-holding по
// timestamp и лупом SetPosition(0) (default; SourceConfig.playOnce=true =
// один проход без seek + Ended()/Render-"ended" в конце). Декод — в нативном
// кадр хранится в frame_); Render отдаёт последний декодированный кадр;
// legacy Render(bgrx, stride) = 720p letterbox (MFT, fallback CPU);
// sized Render = произвольный размер (натив — memcpy, 720p — MFT/CPU,
// остальное — CPU fit). При ошибке открытия/декодирования возвращает false
// с причиной (без «продолжаем прошлый клип»).
class VideoFileSource : public IFrameSource {
public:
    VideoFileSource();
    ~VideoFileSource() override;

    VideoFileSource(const VideoFileSource&) = delete;
    VideoFileSource& operator=(const VideoFileSource&) = delete;

    bool Open(const SourceConfig& cfg, std::wstring& err) override;
    bool Render(uint8_t* bgrx, int stride, std::wstring& err) override;
    bool Render(uint8_t* dst, int stride, uint32_t w, uint32_t h,
                std::wstring& err) override;
    bool NativeSize(uint32_t& w, uint32_t& h) override;
    void Close() override;
    const wchar_t* Name() const override { return L"video"; }
    // true = файл доигран один раз в режиме playOnce (Render даёт false/"ended").
    bool Ended() const override;

private:
    static DWORD WINAPI ThreadProc(LPVOID self);
    void DecodeLoop();
    void SetFailed(const std::wstring& reason);
    // Скейл декодированного кадра в frame_ (натив, кламп к cap): точное
    // совпадение — memcpy, иначе CPU StretchBilinearEx (frame_ — aspect-fit
    // декода, т.е. точная растяжка). MFT-хелпер тут не применим (выход у него
    // фиксированно 720p).
    void RenderToFrame(const BYTE* data, UINT w, UINT h, LONG stride);
    // frame_ -> 720p dst (exact — memcpy, иначе MFT, при false — CPU fit).
    // Вызывать под cs_ при готовом кадре.
    bool ScaleFrameTo720pLocked(BYTE* dst, LONG dstride);
    // Остановка decode-потока с таймаутом: false = поток не успел выйти,
    // никакие ресурсы/состояние не тронуты (иначе — UAF под живым DecodeLoop).
    bool Shutdown(DWORD timeoutMs);

    mutable ATL::CComAutoCriticalSection cs_;
    SourceConfig cfg_;
    ATL::CHandle thread_;
    ATL::CHandle stopEvent_;
    std::vector<uint8_t> frame_; // натив BGRX (кламп к cap), stride frameW_*4
    UINT frameW_ = 0;
    UINT frameH_ = 0;
    bool open_ = false;
    bool frameReady_ = false;
    bool failed_ = false;
    std::wstring failReason_;
    // Режим play-once (из SourceConfig.playOnce): файл играется один раз,
    // на end-of-stream seek НЕ делается, взводится ended_ (Render=false/"ended").
    // Default false = луп SetPosition(0) как раньше.
    bool playOnce_ = false;
    bool ended_ = false;
    VideoProcessorScaler mftScaler_; // GPU-скейл; недоступен -> CPU-fallback
};
