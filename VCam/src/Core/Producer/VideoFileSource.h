#pragma once

#include <windows.h>
#include <atlbase.h>

#include <cstdint>
#include <string>
#include <vector>

#include "ProducerApi.h"
#include "VideoProcessorScaler.h"

// Видеофайл через MF SourceReader: декод в фоновом потоке с frame-holding по
// timestamp. Луп: SourceConfig.loop (video.loop в настройках, default false =
// один проход) && !playOnce (borrow-хоткей отменяет луп); без лупа на конце —
// ended_ + freeze последнего кадра в Render (не NO SIGNAL) с ожиданием
// play-команды (restart с начала) или stop. PlayPauseToggle: ended → restart,
// paused → play, playing → pause (таймлайн замирает, кадр freeze'ится).
// Декод — в нативном кадре хранится в frame_); Render отдаёт последний
// декодированный кадр; legacy Render(bgrx, stride) = 720p letterbox (MFT,
// fallback CPU); sized Render = произвольный размер (натив — memcpy, 720p —
// MFT/CPU, остальное — CPU fit). При ошибке открытия/декодирования возвращает
// false с причиной (без «продолжаем прошлый клип»).
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
    // true = файл доигран (ended_ взведён; Render freeze'ит последний кадр).
    bool Ended() const override;
    // Хоткей play/pause: ended → restart с начала, paused → play, playing →
    // pause. Команда уходит decode-потоку через cmdEvent_ (reader не
    // потокобезопасен — seek только из DecodeLoop).
    void PlayPauseToggle() override;
    bool IsPaused() const override;

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
    // auto-reset: будит DecodeLoop на команду play/pause/restart (пауза и
    // ended замирают на WaitForMultipleObjects{stopEvent_, cmdEvent_}).
    ATL::CHandle cmdEvent_;
    std::vector<uint8_t> frame_; // натив BGRX (кламп к cap), stride frameW_*4
    UINT frameW_ = 0;
    UINT frameH_ = 0;
    bool open_ = false;
    bool frameReady_ = false;
    bool failed_ = false;
    std::wstring failReason_;
    // Луп (SourceConfig.loop && !playOnce): true — seek(0) на end-of-stream.
    // Копируется в локальную на время жизни DecodeLoop (пишется в Open/Shutdown
    // под остановленный поток).
    bool loop_ = false;
    // Режим play-once (из SourceConfig.playOnce): borrow-хоткей — отменяет луп,
    // на EOS взводится ended_.
    bool playOnce_ = false;
    // Конец файла (без лупа): поток жив и ждёт play-команду (restart) или
    // stop; Render freeze'ит последний кадр. Сбрасывается только в DecodeLoop
    // после SetPosition(0) — не трогать из PlayPauseToggle (гонка с Render).
    bool ended_ = false;
    // PlayPauseToggle выставляет под cs_; DecodeLoop замирает до cmd/stop.
    bool paused_ = false;
    // Запрошен restart (с начала): взводится при toggle в ended-состоянии,
    // снимается в DecodeLoop перед SetPosition(0).
    bool restartRequested_ = false;
    VideoProcessorScaler mftScaler_; // GPU-скейл; недоступен -> CPU-fallback
};
