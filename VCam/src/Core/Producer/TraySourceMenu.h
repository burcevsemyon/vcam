#pragma once

#include <windows.h>

#include <string>

#include "Settings.h"

// Результат ApplySourceSwitch: Saved — тип изменён и записан; Unchanged —
// файл уже с этим типом (Save не выполнялся); LoadFailed — файла нет/пуст;
// SaveFailed — запись не прошла.
enum class SourceSwitchResult { Saved, Unchanged, LoadFailed, SaveFailed };

// Подменю «Источник» (Static/Video/Camera, MF_CHECKED по s.sourceType)
// вставляется в parent как MF_POPUP. ID пунктов — параметры.
void AppendSourceSubmenu(HMENU parent, const Settings& s, UINT idStatic,
                         UINT idVideo, UINT idCamera);

// Переключение источника: Load, при совпадении типа — без Save, иначе
// sourceType=type + Save. Guard/лог/borrow — на стороне вызывающего (хост).
SourceSwitchResult ApplySourceSwitch(const std::wstring& path, const wchar_t* type);
