#pragma once

#include <windows.h>
#include <atlbase.h>

#include <cstdint>
#include <string>
#include <vector>

#include "SharedMemoryContract.h"
#include "MappedViewOfFilePtr.h"
#include "VideoProcessorScaler.h"

// Писатель в shared memory: ДВОЙНАЯ запись (фаза vcam-quality-v2/sub1) —
// v1 (Global\VCam.FrameBuffer.v1, 720p letterbox ровно как раньше, бит-в-бит)
// + v2 (Global\VCam.FrameBuffer.v2, натив источника до cap 4K).
// Публикация: seqlock, slot=(idx+1)%N, 30 FPS pacing внутри Write*/FlushLast.
// v1-секция открывается как раньше; v2 — best effort (не открылась — пишем
// только v1, IsV2Open()=false; v1 при этом жив).
// Старый WriteFrame(bgrx, stride) = частный случай входа 720p: хосты пока зовут
// его (Sub3 переведёт на WriteFrameNative); v2 при этом публикует 720p-кадр.
// Если создать Global\ невозможно (Limited-токен без SeCreateGlobalPrivilege) —
// открываем существующую секцию либо создаём сеансовую Local\-копию (читатели
// в SharedMemoryFrameSource перебирают префиксы Global\ -> Local\ сами;
// v2-читатель Sub2 — так же).
class FrameWriter {
public:
    FrameWriter();
    ~FrameWriter();

    FrameWriter(const FrameWriter&) = delete;
    FrameWriter& operator=(const FrameWriter&) = delete;

    bool Open(std::wstring& err);
    void Close();
    bool IsOpen() const { return open_; }
    bool IsV2Open() const { return v2open_; }
    // Какое имя секции реально использовалось (для лога хоста).
    const std::wstring& SectionOpenedAs() const { return openedSection_; }
    const std::wstring& V2SectionOpenedAs() const { return openedSectionV2_; }

    // Качество v2: L"source" (натив, default) | L"fixed1080p" (v2 = 1080p,
    // лесенка только вниз) | L"fixed720p" (v2 = 720p). Одно на все источники:
    // писатель подгоняет каждый источник под общий размер (fit/letterbox).
    // Остальное нормализуется в source. Хосты (Sub3) выставляют из Settings.
    void SetQuality(const std::wstring& q)
    {
        quality_ = (q == L"fixed720p") ? L"fixed720p"
            : (q == L"fixed1080p")     ? L"fixed1080p"
                                       : L"source";
    }
    const std::wstring& Quality() const { return quality_; }

    // Чистая математика целевого размера v2 (без shm — для harness):
    // source -> fit-кламп входа к cap; fixed1080p -> fit-кламп к 1920x1080
    // (меньше — passthrough, лесенка только вниз); fixed720p -> 1280x720.
    // false = мусор входа.
    static bool ResolveV2Size(uint32_t srcW, uint32_t srcH, const std::wstring& quality,
                              uint32_t& outW, uint32_t& outH)
    {
        if (srcW == 0 || srcH == 0 || srcW > 8192 || srcH > 8192) return false;
        if (quality == L"fixed720p") {
            outW = vcam::VCamWidth;
            outH = vcam::VCamHeight;
            return true;
        }
        if (quality == L"fixed1080p") {
            // Cap ступени 1080p — константа (в контракте её нет, UI нет).
            constexpr uint32_t capW = 1920, capH = 1080;
            if (srcW <= capW && srcH <= capH) {
                outW = srcW;
                outH = srcH;
                return true;
            }
            double s = (double)capW / srcW;
            double s2 = (double)capH / srcH;
            if (s2 < s) s = s2;
            outW = (uint32_t)(srcW * s + 0.5);
            outH = (uint32_t)(srcH * s + 0.5);
            if (outW < 1) outW = 1;
            if (outH < 1) outH = 1;
            if (outW > capW) outW = capW;
            if (outH > capH) outH = capH;
            return true;
        }
        if (srcW <= vcam::VCamNativeCapW && srcH <= vcam::VCamNativeCapH) {
            outW = srcW;
            outH = srcH;
            return true;
        }
        double s = (double)vcam::VCamNativeCapW / srcW;
        double s2 = (double)vcam::VCamNativeCapH / srcH;
        if (s2 < s) s = s2;
        outW = (uint32_t)(srcW * s + 0.5);
        outH = (uint32_t)(srcH * s + 0.5);
        if (outW < 1) outW = 1;
        if (outH < 1) outH = 1;
        if (outW > vcam::VCamNativeCapW) outW = vcam::VCamNativeCapW;
        if (outH > vcam::VCamNativeCapH) outH = vcam::VCamNativeCapH;
        return true;
    }

    // Кладёт 720p-кадр в shared memory и кэширует его как последний удачный
    // (v1 + v2-720p). Вход — packed 1280x720 BGRX, логика v1 без изменений.
    bool WriteFrame(const uint8_t* bgrx, int stride);
    // Кладёт НАТИВНЫЙ кадр WxH BGRX (stride >= W*4, top-down): v1 — даунскейл
    // до 720p letterbox (MFT, fallback CPU), v2 — по лесенке quality (натив,
    // 1080p или 720p). Пишет обе секции, кэширует обе.
    bool WriteFrameNative(const uint8_t* bgrx, int stride, uint32_t w, uint32_t h);
    // Повторно публикует последний удачный кадр (hot-switch, пока новый источник
    // не готов). false — кадра ещё не было / writer не открыт.
    bool FlushLast();
    // Последний опубликованный 720p-кадр v1 (packed 1280x720 BGRX top-down,
    // stride 5120 — ровно то, что ушло в эфир, включая эффекты хоста).
    // Пустой вектор = кадра ещё не было. Читатель — тот же worker-поток хоста,
    // что зовёт Write* (доп. синхронизация не нужна); для записи эфира в файл.
    bool HasFrame720p() const { return hasFrame_; }
    const std::vector<uint8_t>& LastFrame720p() const { return cache_; }

private:
    bool PublishLocked();
    bool PublishLockedV2();
    // Источник кадра v2: при зеркале 720p байты уже лежат в cache_ — копия в
    // cacheV2_ не нужна; FlushLast повторяет тот же выбор.
    const uint8_t* V2SrcLocked() const;
    void Pace();
    bool OpenV2(std::wstring& err, SECURITY_ATTRIBUTES* sa);
    void CloseV2();

    mutable ATL::CComAutoCriticalSection cs_;
    ATL::CHandle hSection_;
    ATL::CHandle hReady_;
    PSECURITY_DESCRIPTOR pSecDesc_ = nullptr;
    vcam::MappedViewOfFilePtr view_;
    vcam::VCamSectionHeader* pHeader_ = nullptr;
    std::vector<uint8_t> cache_;
    bool hasFrame_ = false;
    bool open_ = false;
    std::wstring openedSection_;
    // v2 (best effort)
    ATL::CHandle hSectionV2_;
    ATL::CHandle hReadyV2_;
    vcam::MappedViewOfFilePtr viewV2_;
    vcam::VCamSectionHeader* pHeaderV2_ = nullptr;
    std::vector<uint8_t> cacheV2_; // packed натив (stride v2Stride_), max 33МБ
    uint32_t v2W_ = 0;
    uint32_t v2H_ = 0;
    uint32_t v2Stride_ = 0;
    bool hasV2Frame_ = false;
    bool v2open_ = false;
    std::wstring openedSectionV2_;
    VideoProcessorScaler mftScaler_; // натив -> 720p; недоступен -> CPU-fallback
    std::wstring quality_ = L"source";
    LARGE_INTEGER freq_ = {};
    LONGLONG startCount_ = 0;
    LONGLONG lastEmit_ = 0;
    HANDLE hPaceTimer_ = nullptr; // high-resolution waitable timer для Pace()
};
