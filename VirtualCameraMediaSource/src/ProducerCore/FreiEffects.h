#pragma once

#include <cstdint>
#include <string>

// Backend помех на frei0r-плагинах (динамическая загрузка DLL).
// Контракт: in-place над плотно упакованным BGRX-кадром (stride == w*4),
// вызывается из GpuEffects::ApplyEffects, когда FxFlags.backend == "frei0r".
// Нет DLL / init fail / AV в плагине → kNeedCpu: вызывающий прогоняет
// штатный CPU-путь (кадр пишется всегда, хост даёт one-shot лог).
namespace vcam::effects::frei {

enum class FreiResult {
    kApplied, // цепочка плагинов отработала, кадр изменён
    kNeedCpu, // плагины недоступны/сломаны — вызывающий обязан прогнать CPU
    kFailed,  // плохие аргументы или OOM — вызывающий делает fail-open
};

struct AnalogRequest {
    bool noise = false;
    bool scanlines = false;
    bool rgbSplit = false;
    bool tracking = false;
    // Третья тройка (только frei0r-backend, CPU-аналогов нет):
    bool gateweave = false; // дрожание плёнки (gateweave.dll)
    bool glow = false;      // свечение светов (glow.dll)
    bool denoise = false;   // шумодав hqdn3d (denoise_hqdn3d.dll)
    int noiseLevel = 100;
    int scanlinesLevel = 100;
    int rgbSplitLevel = 100;
    int trackingLevel = 100;
    int gateweaveLevel = 100;
    int glowLevel = 100;
    int denoiseLevel = 100;
    double timeSec = 0.0; // время кадра для f0r_update (кадр/30)
};

FreiResult ApplyAnalog(uint8_t* bgrx, uint32_t w, uint32_t h,
                       const AnalogRequest& req);

// One-shot флаг CPU-fallback: true, если ХОТЯ БЫ один вызов с прошлого
// чтения ушёл в kNeedCpu. Обмен со сбросом (для лога хоста).
bool TakeCpuFallbackFlag();

// Для тестов: искать DLL только в этом каталоге (иначе exe-dir\frei0r,
// exe-dir, FREI0R_PATH).
void SetSearchDir(const std::wstring& dir);

void ShutdownFrei();

} // namespace vcam::effects::frei
