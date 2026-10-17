#pragma once

#include <windows.h>
#include <atlbase.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <numeric>
#include <mutex>
#include <string>
#include <vector>

#include "ProducerApi.h"
#include "SharedMemoryContract.h"
#include "VideoProcessorScaler.h"
#include "CameraControls.h"
#include "ControlServer.h"

struct IMFSourceReader;
struct IMFMediaSource;

// Чистые helpers NV12-захвата (sub vcam-camera-nv12-capture): без MF,
// тестируются harness'ом в %TEMP% включением этого header'а (без устройства).
// Реальный CaptureLoop/переговоры в CameraSource.cpp используют ИХ ЖЕ
// (один источник — зеркала формулы нет).
namespace vcam::camsource {

// BT.601 limited-range integer NV12->RGB32 (BGRX, A=255) с clamp.
// src: Y-плоскость w*h (stride srcStride байт/строка, бывает с паддингом,
// может быть отрицательным — bottom-up), затем interleaved UV
// ((w/2*2) байт/строка, (h+1)/2 строк ceil — тот же stride).
// dst: RGB32 BGRX top-down (dstStride байт/строка; отрицательный — bottom-up).
// Нечётные w/h: UV-индекс clamp'ится к последнему доступному (край повторяется).
inline void Nv12ToRgb32(const uint8_t* src, UINT32 w, UINT32 h, LONG srcStride,
                         uint8_t* dst, LONG dstStride)
{
    if (!src || !dst || w == 0 || h == 0) return;
    LONG sStride = (srcStride == 0) ? (LONG)w : srcStride;
    LONG dStride = (dstStride == 0) ? (LONG)(w * 4u) : dstStride;
    const size_t sAbs = (size_t)(sStride >= 0 ? sStride : -sStride);
    const size_t dAbs = (size_t)(dStride >= 0 ? dStride : -dStride);
    if (sAbs == 0 || dAbs < (size_t)w * 4u) return;
    const uint8_t* yBase = src;
    const uint8_t* uvBase = src + sAbs * (size_t)h;
    const UINT32 uvH = (h + 1u) / 2u;
    for (UINT32 y = 0; y < h; y++) {
        const uint8_t* yRow = (sStride >= 0)
            ? yBase + (size_t)y * sAbs
            : yBase + (size_t)(h - 1u - y) * sAbs;
        const UINT32 uvY = y >> 1;
        const uint8_t* uvRow = (sStride >= 0)
            ? uvBase + (size_t)uvY * sAbs
            : uvBase + (size_t)(uvH - 1u - uvY) * sAbs;
        uint8_t* dRow = (dStride >= 0)
            ? dst + (size_t)y * dAbs
            : dst + (size_t)(h - 1u - y) * dAbs;
        for (UINT32 x = 0; x < w; x++) {
            const int Y = (int)yRow[x];
            UINT32 uvX = (x >> 1) << 1;
            if (uvX >= sAbs) uvX = (UINT32)(sAbs - 1u) & ~1u;
            const int U = (int)uvRow[uvX];
            const int V = (uvX + 1u < sAbs) ? (int)uvRow[uvX + 1u] : (int)uvRow[uvX];
            // BT.601 limited range: Y'CbCr -> RGB, коэффициенты с точностью до 1/256
            const int yMinus16 = Y - 16;
            const int uMinus128 = U - 128;
            const int vMinus128 = V - 128;
            int R = (298 * yMinus16 + 409 * vMinus128 + 128) >> 8;
            int G = (298 * yMinus16 - 100 * uMinus128 - 208 * vMinus128 + 128) >> 8;
            int B = (298 * yMinus16 + 516 * uMinus128 + 128) >> 8;
            if (R < 0) R = 0; else if (R > 255) R = 255;
            if (G < 0) G = 0; else if (G > 255) G = 255;
            if (B < 0) B = 0; else if (B > 255) B = 255;
            dRow[(size_t)x * 4u + 0u] = (uint8_t)B;
            dRow[(size_t)x * 4u + 1u] = (uint8_t)G;
            dRow[(size_t)x * 4u + 2u] = (uint8_t)R;
            dRow[(size_t)x * 4u + 3u] = 255;
        }
    }
}

// Нормализация camera.capture (формула size-sub 1:1): только 720p/1080p
// проходят, остальное = max (0 = без цели).
inline UINT32 CaptureTargetHeightFor(const std::wstring& capture)
{
    if (capture == L"720p") return 720;
    if (capture == L"1080p") return 1080;
    return 0;
}

// Score размера (формула size-sub 1:1, от субтипа НЕ зависит):
// max (0): -area в cap, 4e9+area за cap; иначе dh*1e8-area в cap.
inline int64_t NativeScoreFor(UINT32 w, UINT32 h, UINT32 targetH)
{
    const int64_t area = (int64_t)w * h;
    if (targetH == 0) {
        if (w <= vcam::VCamNativeCapW && h <= vcam::VCamNativeCapH)
            return -area;
        return (int64_t)4000000000LL + area;
    }
    const int64_t dh = llabs((int64_t)h - (int64_t)targetH);
    if (w <= vcam::VCamNativeCapW && h <= vcam::VCamNativeCapH)
        return dh * (int64_t)100000000LL - area;
    return (int64_t)4000000000LL + dh * (int64_t)100000000LL + area;
}

// Класс субтипа для NV12-first порядка (выходной субтип — только NV12/RGB32,
// MJPG выходом НЕ бывает; размеры MJPG в RGB32-фолбэке оставлены как в старом
// пути — см. комментарий в ConfigureCameraReader).
enum class CamSub : int { NV12 = 0, RGB32 = 1, Other = 2, Mjpg = 3 };
struct CamTry { CamSub sub; UINT32 w; UINT32 h; };

// Индексы в try-порядке по score (stable — равные сохраняют входной порядок).
inline std::vector<size_t> SortIndicesByScore(const std::vector<CamTry>& tries,
                                              UINT32 targetH)
{
    std::vector<size_t> idx(tries.size());
    std::iota(idx.begin(), idx.end(), size_t{ 0 });
    std::stable_sort(idx.begin(), idx.end(), [&](size_t a, size_t b) {
        return NativeScoreFor(tries[a].w, tries[a].h, targetH) <
               NativeScoreFor(tries[b].w, tries[b].h, targetH);
    });
    return idx;
}

// NV12-подмножество score-порядка (Phase A); пусто = NV12-кандидатов нет.
inline std::vector<size_t> Nv12IndicesInOrder(const std::vector<CamTry>& tries,
                                              const std::vector<size_t>& sorted)
{
    std::vector<size_t> out;
    out.reserve(sorted.size());
    for (size_t i : sorted)
        if (i < tries.size() && tries[i].sub == CamSub::NV12) out.push_back(i);
    return out;
}

// Тик NATIVEMEDIATYPECHANGED: нулевой сэмпл с этим флагом (0x100 —
// MF_SOURCE_READERF_NATIVEMEDIATYPECHANGED). НЕ молчание и НЕ смерть.
inline bool IsNativeTypeChangedTick(bool hasSample, DWORD flags)
{
    return !hasSample && (flags & 0x100u) != 0u;
}

// Нужна ли адаптация к текущему типу (размеры/субтип разошлись).
inline bool ShouldAdaptToCurrent(UINT32 capW, UINT32 capH, int capSubId,
                                 UINT32 curW, UINT32 curH, int curSubId)
{
    return capW != curW || capH != curH || capSubId != curSubId;
}

} // namespace vcam::camsource

// Захват с физической камеры через MF SourceReader. Фоновый поток читает
// сэмплы (RGB32 capW x capH напрямую либо NV12-натив capW x capH с CPU-конвертом
// BT.601 в RGB32-кэш) в кэш последнего кадра под мьютексом; Render
// отдаёт кэш в буфер хоста (построчный memcpy при 1280x720, иначе сначала
// Video Processor MFT, при его недоступности — letterbox-fit билинейный). Ошибка/молчание камеры ~3 с -> failed_ и Render=false с причиной
// (хост уходит в fallback NO SIGNAL; никакого keep-previous).
// Нулевой сэмпл с флагом NATIVEMEDIATYPECHANGED (0x100) — тик смены натива:
// НЕ молчание (silent сбрасывается), перечитываем GetNative/GetCurrent и при
// расхождении адаптируемся (размеры/субтип + resize кэша) без failed.
class CameraSource : public IFrameSource {
public:
    CameraSource();
    ~CameraSource() override;

    CameraSource(const CameraSource&) = delete;
    CameraSource& operator=(const CameraSource&) = delete;

    bool Open(const SourceConfig& cfg, std::wstring& err) override;
    bool Render(uint8_t* bgrx, int stride, std::wstring& err) override;
    bool Render(uint8_t* dst, int stride, uint32_t w, uint32_t h,
                std::wstring& err) override;
    bool NativeSize(uint32_t& w, uint32_t& h) override;
    void Close() override;
    const wchar_t* Name() const override { return L"camera"; }

private:
    static DWORD WINAPI ThreadProc(LPVOID self);
    void CaptureLoop();
    // Тик 0x100: перечитать GetNativeMediaType/GetCurrentMediaType и при
    // расхождении с capW_/capH_/capSub_ — адаптироваться (обновить + resize
    // кэша под мьютексом + лог). Вызывается из CaptureLoop (reader валиден:
    // Shutdown ждёт выхода потока до release). failed_ НЕ выставляется.
    void OnNativeTypeChangedTick(IMFSourceReader* rdr);
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
    LONG capStride_ = 0;         // stride источника (может быть отрицательным;
                                 // NV12 — stride Y-плоскости, бывает с паддингом)
    GUID capSub_ = {};             // выходной субтип (NV12 либо RGB32/ARGB32)
    bool capIsNv12_ = false;     // true = сэмплы NV12, конверт CPU в RGB32-кэш
    bool open_ = false;
    bool hasFrame_ = false;
    bool failed_ = false;
    bool mfUp_ = false;          // парный MFStartup/MFShutdown
    bool comUp_ = false;         // парный CoInitializeEx/CoUninitialize
    std::wstring failReason_;
    VideoProcessorScaler mftScaler_; // GPU-скейл кэша; недоступен -> CPU-fallback
    // Управление: QI IAM* с нашего же IMFMediaSource (второй ActivateObject не
    // нужен) + pipe-сервер `\\.\pipe\VCamControl.v1` для виртуалки (Sub 2).
    // Старт в Open (best effort — ошибка не роняет источник), стоп в Shutdown.
    CameraControls controls_;
    ControlServer controlServer_;
};
