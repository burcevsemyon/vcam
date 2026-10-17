#pragma once

#include <cstdint>
#include <memory>
#include <string>

// Десериализованный конфиг активного источника (см. Settings — новая схема).
struct SourceConfig {
    std::wstring type;         // L"static" | L"video" | L"camera"
    std::wstring path;         // static/video: путь к файлу; camera: id (symlink)
    std::wstring camName;      // camera: friendly name (если path пуст — поиск по имени)
    std::wstring capture = L"max"; // camera: L"max" (def, лучшее в cap) | L"720p" | L"1080p"
    std::wstring scaleMode;    // L"fit" | L"cover" | L"crop" (static)
    int cropX = 0;
    int cropY = 0;
    int cropW = 0;
    int cropH = 0;
    bool cropKeepAspect = false;
    // Хоткей static→video: true = проиграть video один раз и сообщить Ended()
    // (хост вернёт запомненный источник). false = луп как раньше.
    bool playOnce = false;
    // video.loop (секция настроек): true = крутить ролик по кругу. false
    // (default) = один проход, на конце — ended_ + freeze последнего кадра
    // (не NO SIGNAL). playOnce (borrow) отменяет луп независимо от этого флага.
    bool loop = false;

    bool operator==(const SourceConfig& o) const
    {
        return type == o.type && path == o.path && camName == o.camName &&
                capture == o.capture && scaleMode == o.scaleMode &&
                cropX == o.cropX && cropY == o.cropY && cropW == o.cropW &&
                cropH == o.cropH && cropKeepAspect == o.cropKeepAspect &&
                playOnce == o.playOnce && loop == o.loop;
    }
    bool operator!=(const SourceConfig& o) const { return !(*this == o); }
};

// Источник кадров BGRX (верхняя строка первая).
// Legacy Render(bgrx, stride) = кадр 1280x720 (stride >= vcam::VCamStride) —
// путь хостов до Sub3-перевода (v1). Sized Render + NativeSize = натив
// источника (v2): размеры переменные, cap 3840x2160 (vcam::VCamNativeCapW/H).
struct IFrameSource {
    virtual bool Open(const SourceConfig& cfg, std::wstring& err) = 0;
    // false = данных сейчас нет (ещё не открыт / нет кадра / ошибка) — вызывающий
    // не должен писать такой кадр в shared memory.
    virtual bool Render(uint8_t* bgrx, int stride, std::wstring& err) = 0;
    // Рендер в буфер произвольного размера WxH BGRX (stride >= W*4, top-down).
    virtual bool Render(uint8_t* dst, int stride, uint32_t w, uint32_t h,
                        std::wstring& err) = 0;
    // Нативные размеры источника (кламп к cap). false = неизвестны
    // (не открыт / первый кадр ещё не готов) — вызывающий ждёт/фолбэчит 720p.
    virtual bool NativeSize(uint32_t& w, uint32_t& h) = 0;
    // Конец воспроизведения (video доиграло файл: ended_ взведён, Render
    // freeze'ит последний кадр). Default false; луп-режим никогда не
    // заканчивается. Borrow-автовозврат хоста опирается на этот флаг.
    virtual bool Ended() const { return false; }
    // Play/pause toggle по хоткею: video — ended → restart с начала,
    // paused → play, playing → pause; остальные источники — no-op.
    virtual void PlayPauseToggle() {}
    // true = воспроизведение приостановлено (для логов/наблюдаемости).
    virtual bool IsPaused() const { return false; }
    virtual void Close() = 0;
    virtual const wchar_t* Name() const = 0;
    virtual ~IFrameSource() = default;
};

// L"static" -> StaticImageSource, L"video" -> VideoFileSource,
// L"camera" -> CameraSource, иное -> nullptr.
std::unique_ptr<IFrameSource> CreateSource(const std::wstring& type);
