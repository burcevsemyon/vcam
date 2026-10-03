#pragma once

#include <cstdint>

// Покадровые эффекты хоста на GPUPixel (замена самописного Effects.h).
// Контракт вызова прежний: in-place над BGRX-кадром после RenderOne,
// перед WriteOne. Settings/UI (`effects {mirror,grayscale}`) не меняются.
namespace vcam::effects {

// Применяет эффекты к кадру. Возвращает:
//   true  — эффекты применены (или оба флага выключены — кадр не тронут);
//   false — GPU/библиотека недоступны: кадр ОСТАВЛЕН БЕЗ ИЗМЕНЕНИЙ
//           (fail-open, камера обязана работать). Вызывающий логирует один раз.
// Только плотно упакованные кадры (stride == w*4, как m.buf хоста);
// прочий stride — тоже false без изменений.
bool ApplyEffects(uint8_t* bgrx, int stride, uint32_t w, uint32_t h,
                  bool mirror, bool grayscale);

// Освобождение пайплайна (best-effort). Скрытый GL-контекст и worker-поток
// принадлежат библиотеке и живут до конца процесса (дизайн GPUPixel:
// GPUPixelContext::Destroy — внутренний API, в публичных хедерах его нет).
void ShutdownEffects();

} // namespace vcam::effects
