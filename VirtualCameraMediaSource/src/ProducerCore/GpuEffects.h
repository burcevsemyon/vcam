#pragma once

#include <cstdint>
#include <string>

// Покадровые эффекты хоста: GPUPixel (зеркало + Ч/Б) + помехи (backend).
// Контракт вызова прежний: in-place над BGRX-кадром после RenderOne,
// перед WriteOne.
namespace vcam::effects {

// Флаги эффектов — 1:1 с Settings::EffectsSection (Settings.h) и C# Settings.
// Уровни 0–100 (default 100 = вид как без уровней; 0 при включённом тоггле
// ≈ эффект выключен). VHS использует индивидуальные уровни четырёх помех.
// backend: L"cpu" (default, без DLL) | L"frei0r" (цепочка frei0r-плагинов;
// нет DLL/init fail → fail-open на CPU + one-shot лог хоста).
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
    std::wstring backend = L"cpu";
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

// One-shot флаг CPU-fallback frei0r (обмен со сбросом): true, если хотя бы
// один кадр с прошлого чтения посчитан CPU из-за недоступных DLL.
// Хост логирует один раз; CPU-путь при этом бит-в-бит как backend cpu.
bool TakeFreiFallbackFlag();

} // namespace vcam::effects
