#pragma once

// Единая политика ротации логов (backlog observability.md P0.4).
// Если файл превысил cap — переименовать в <path>.old (старый .old удалить),
// запись продолжить в свежий <path>. Header-only, per-module cap. Безопасно
// вызывать перед каждой записью в лог. Ротация best-effort: если файл залочен
// читателем — MoveFileW не удаётся, ротация повторится на следующей записи.

#include <windows.h>

#include <string>

namespace vcam {

// Cap'ы на лог (после превышения — ротация в .old). Единая схема — per-log объём.
inline constexpr uint64_t kHostLogCapBytes = 1024ull * 1024ull;     // 1 МБ
inline constexpr uint64_t kMsrcDiagCapBytes = 10ull * 1024 * 1024;  // 10 МБ

// Ротация лога по cap. Возвращает true, если можно писать (ротация выполнена
// или не требуется), false — если файл залочен и ротировать не удалось.
inline bool RotateLogIfOverCap(const std::wstring& path, uint64_t capBytes)
{
    if (path.empty()) return true;
    WIN32_FILE_ATTRIBUTE_DATA a = {};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a)) return true;
    ULARGE_INTEGER sz = {};
    sz.LowPart = a.nFileSizeLow;
    sz.HighPart = a.nFileSizeHigh;
    if (sz.QuadPart <= capBytes) return true;
    const std::wstring old = path + L".old";
    DeleteFileW(old.c_str());
    return MoveFileW(path.c_str(), old.c_str()) != 0;
}

} // namespace vcam
