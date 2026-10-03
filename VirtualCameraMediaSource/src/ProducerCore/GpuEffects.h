#pragma once

#include <cstdint>

// Покадровые эффекты хоста: GPUPixel (зеркало + Ч/Б) + CPU-аналог помех.
// Контракт вызова прежний: in-place над BGRX-кадром после RenderOne,
// перед WriteOne.
namespace vcam::effects {

// Флаги эффектов — 1:1 с Settings::EffectsSection (Settings.h) и C# Settings.
// Уровни 0–100 (default 100 = вид как без уровней; 0 при включённом тоггле
// ≈ эффект выключен). VHS использует индивидуальные уровни четырёх помех.
struct FxFlags {
    bool mirror = false;
    bool grayscale = false;
    bool noise = false;     // RGB/белый шум
    bool scanlines = false; // чересстрочные линии
    bool rgbSplit = false;  // хроматическая аберрация (RGB-сдвиг)
    bool tracking = false;  // трекинг-глитч (сдвинутые полосы)
    bool vhs = false;       // VHS-пресет: все четыре помехи сразу
    int noiseLevel = 100;
    int scanlinesLevel = 100;
    int rgbSplitLevel = 100;
    int trackingLevel = 100;
};

// Применяет эффекты к кадру. Возвращает:
//   true  — эффекты применены (или все флаги выключены — кадр не тронут);
//   false — сбой: кадр ОСТАВЛЕН БЕЗ ИЗМЕНЕНИЙ (fail-open, камера обязана
//           работать). Вызывающий логирует один раз.
// Порядок: GPU (mirror+grayscale, существующие пайпы) → CPU-аналог
// (rgbSplit → tracking → noise → scanlines). Цветной шум поверх Ч/Б —
// задуманный chroma-noise; rgb-split на сером даёт цветные кромки.
// GPU запрошен и упал → CPU пропускается (атомарно всё-или-ничего).
// Только плотно упакованные кадры (stride == w*4, как m.buf хоста);
// прочий stride — тоже false без изменений.
bool ApplyEffects(uint8_t* bgrx, int stride, uint32_t w, uint32_t h,
                  const FxFlags& fx);

// Освобождение пайплайна (best-effort). Скрытый GL-контекст и worker-поток
// принадлежат библиотеке и живут до конца процесса (дизайн GPUPixel:
// GPUPixelContext::Destroy — внутренний API, в публичных хедерах его нет).
void ShutdownEffects();

} // namespace vcam::effects
