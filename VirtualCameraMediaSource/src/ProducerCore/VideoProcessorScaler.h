#pragma once

#include <windows.h>

#include <cstdint>

// Скейлер 1280x720 BGRX через Video Processor MFT (software, sysmem) с
// CPU-fallback у вызывающего. Контракт shared memory не меняется: выход всегда
// packed top-down 1280x720, stride 5120.
//
// D3D-менеджер осознанно НЕ аттачим: VP MFT в D3D-режиме требует surface-сэмплы
// и отклоняет sysmem-вход E_NOINTERFACE (замерено). GPU-путь потребовал бы
// DXGI upload/download — вне скоупа; software MFT даёт convert+scale.
// Использование: точное совпадение 1280x720 вызывающий копирует сам (memcpy);
// несовпадение — сначала Scale/ScaleEx, при false — LetterboxBilinear.
// StaticImageSource (WIC) этот хелпер не использует.
class VideoProcessorScaler {
public:
    VideoProcessorScaler();
    ~VideoProcessorScaler();

    VideoProcessorScaler(const VideoProcessorScaler&) = delete;
    VideoProcessorScaler& operator=(const VideoProcessorScaler&) = delete;

    // Вход RGB32/ARGB32 (4 байта/пиксел, layout идентичен): src stride в байтах
    // (может быть отрицательным — bottom-up), dst — буфер 1280*720*4.
    // false = MFT недоступен или кадр не обработан — вызывающий уходит
    // на CPU-fallback. Потокобезопасно (внутренний мьютекс).
    bool Scale(const BYTE* src, UINT w, UINT h, LONG stride, BYTE* dst);

    // Явный сабтайп входа (MFVideoFormat_RGB32/ARGB32/NV12/YUY2). Layout src
    // обязан соответствовать сабтайпу: NV12 — Y-плоскость stride*h + UV
    // interleaved stride*h/2 сверху вниз; YUY2 — packed 2 байта/пиксел.
    bool ScaleEx(const BYTE* src, UINT w, UINT h, LONG stride,
                 const GUID& subtype, BYTE* dst);

    // Сброс MFT-состояния: FLUSH + END_STREAMING + release (идемпотентно).
    // Вызывать ДО MFShutdown, пока платформа жива (см. CameraSource::Shutdown):
    // иначе teardown платформы с живым MFT роняет процесс на выходе.
    void Shutdown();

private:
    bool EnsureInit();
    bool Configure(UINT w, UINT h, const GUID& subtype);
    bool Process(const BYTE* src, LONG stride, const GUID& subtype, BYTE* dst);
    // Одна попытка Input->Output. rejected=true: вход отклонён (MFT держит
    // предыдущий) — caller делает FLUSH и ровно один ретрай.
    bool ProcessOnce(const BYTE* src, LONG stride, const GUID& subtype, BYTE* dst,
                     bool& rejected);
    void TeardownLocked(); // FLUSH + END + release, сброс cfg (мьютекс взят)

    void* mft_ = nullptr;   // IMFTransform* (void* чтобы не тянуть MF в header)
    void* ctrl_ = nullptr;  // IMFVideoProcessorControl*
    void* mutex_ = nullptr; // std::mutex* (pimpl чтобы не тянуть <mutex> в header)

    bool unavailable_ = false;
    bool streaming_ = false;
    GUID cfgSubtype_ = GUID_NULL;
    UINT cfgW_ = 0;
    UINT cfgH_ = 0;
    LONG outStride_ = 0;
    unsigned long outBufSize_ = 0;
    long long frameIndex_ = 0; // монотонные таймстампы сэмплов (30 FPS)
};
