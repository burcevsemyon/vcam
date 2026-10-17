#pragma once

// Единый форматтер строк лога VCam (observability.md P0.1).
// Шаблон: [YYYY-MM-DD HH:MM:SS.mmm] [уровень] [область] сообщение
// Время — локальное (GetLocalTime), как было в host.log/msrc_diag historically.
//
// Header-only, БЕЗ глобального состояния и кучи: FormatLogPrefix пишет в
// буфер вызывающего через GetLocalTime + _snwprintf_s — безопасно вызывать
// из-под loader lock (DllMain MediaSource) и на путях 30 FPS pacing.

#include <windows.h>

#include <cstdio>
#include <string>

namespace vcam {

enum class LogLevel { Debug, Info, Warn, Error };

// Короткий тег уровня для поля [уровень].
inline const wchar_t* LogLevelTag(LogLevel level)
{
    switch (level) {
    case LogLevel::Debug: return L"debug";
    case LogLevel::Info:  return L"info";
    case LogLevel::Warn:  return L"warn";
    case LogLevel::Error: return L"error";
    }
    return L"info";
}

// Пишет префикс "[YYYY-MM-DD HH:MM:SS.mmm] [level] [area] " в buf.
// Возвращает число записанных символов (без '\0'), либо -1 при усечении.
// Никакой кучи/глобального состояния — только стек + GetLocalTime.
inline int FormatLogPrefix(wchar_t* buf, int bufCount, LogLevel level,
                           const wchar_t* area)
{
    if (buf == nullptr || bufCount <= 0) return -1;
    SYSTEMTIME st = {};
    GetLocalTime(&st);
    return _snwprintf_s(buf, bufCount, _TRUNCATE,
                        L"[%04u-%02u-%02u %02u:%02u:%02u.%03u] [%s] [%s] ",
                        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
                        st.wSecond, st.wMilliseconds, LogLevelTag(level), area);
}

// VCAM_DEBUG=1 включает debug-строки во всех компонентах (P1.1).
// Читается один раз при первом вызове (кэш); для включения нужен рестарт
// процесса (host/CLI) или перезагрузка DLL (рестарт FrameServer для msrc).
// Выключен по умолчанию: без флага debug-строки не тратят время на форматирование.
inline bool IsDebugEnabled()
{
    static bool enabled = [] {
        wchar_t v[16] = {};
        DWORD n = GetEnvironmentVariableW(L"VCAM_DEBUG", v, 16);
        return n == 1 && v[0] == L'1';
    }();
    return enabled;
}

// Удобная обёртка для хоста/CLI (вне loader lock): префикс + сообщение одной
// строкой. MediaSource (DllMain) использует FormatLogPrefix напрямую в буфер.
inline std::wstring FormatLogLine(LogLevel level, const wchar_t* area,
                                  const std::wstring& msg)
{
    wchar_t prefix[64] = {};
    int n = FormatLogPrefix(prefix, 64, level, area);
    if (n < 0) n = 0;
    return std::wstring(prefix, (size_t)n) + msg;
}

} // namespace vcam
