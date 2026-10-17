#pragma once

// Минидампы на необработанное исключение (backlog observability.md P2.1).
// Handler + MiniDumpWriteDump (без реестра/adminkи, работает per-process).
// MediaSource (in-proc в чужом svchost) — НЕ покрывается (там P2.2 + хвост лога).
// Header-only. В фильтре падения — ТОЛЬКО стековые буферы и kernel-вызовы
// (куча/CRT могут быть повреждены в момент AV).

#include <windows.h>
#include <DbgHelp.h>

#include <cstdio>

#pragma comment(lib, "Dbghelp.lib") // авто-линк, без правок vcxproj

namespace vcam {
namespace detail {

inline const wchar_t*& CrashTagStorage()
{
    static const wchar_t* tag = L"app";
    return tag;
}

// Фильтр необработанных SEH-исключений: пишет .dmp и гасит процесс.
// Возвращает EXCEPTION_EXECUTE_HANDLER (без WER-UI — для tray/CLI тихо).
inline LONG WINAPI CrashFilter(EXCEPTION_POINTERS* xp)
{
    wchar_t base[MAX_PATH] = {};
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return EXCEPTION_EXECUTE_HANDLER;
    wchar_t dir[MAX_PATH] = {};
    _snwprintf_s(dir, _countof(dir), _TRUNCATE, L"%s\\VCam\\Crashes", base);
    CreateDirectoryW(dir, nullptr); // на случай, если Install не успел
    SYSTEMTIME st = {};
    GetLocalTime(&st);
    wchar_t path[MAX_PATH] = {};
    _snwprintf_s(path, _countof(path), _TRUNCATE, L"%s\\%s_%u_%04u%02u%02u_%02u%02u%02u.dmp",
        dir, CrashTagStorage(), (unsigned)GetCurrentProcessId(),
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION mei = {};
        mei.ThreadId = GetCurrentThreadId();
        mei.ExceptionPointers = xp;
        mei.ClientPointers = FALSE;
        MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), f,
            (MINIDUMP_TYPE)(MiniDumpWithDataSegs | MiniDumpWithHandleData |
                            MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules |
                            MiniDumpWithIndirectlyReferencedMemory),
            &mei, nullptr, nullptr);
        CloseHandle(f);
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

} // namespace detail

// Чистка старых дампов (оставить newest 5). Вызывать на старте, НЕ в фильтре.
inline void PruneOldDumps()
{
    wchar_t base[MAX_PATH] = {};
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return;
    wchar_t dir[MAX_PATH] = {};
    _snwprintf_s(dir, _countof(dir), _TRUNCATE, L"%s\\VCam\\Crashes", base);
    CreateDirectoryW(dir, nullptr);
    wchar_t mask[MAX_PATH] = {};
    _snwprintf_s(mask, _countof(mask), _TRUNCATE, L"%s\\*.dmp", dir);
    // Собираем (имя, время) — дампов мало, линейный отбор достаточен.
    struct Entry { wchar_t name[MAX_PATH]; ULARGE_INTEGER t; };
    Entry entries[256] = {};
    int count = 0;
    WIN32_FIND_DATAW fd = {};
    HANDLE fh = FindFirstFileW(mask, &fd);
    if (fh == INVALID_HANDLE_VALUE) return;
    do {
        if (count < 256) {
            wcsncpy_s(entries[count].name, fd.cFileName, _TRUNCATE);
            entries[count].t.LowPart = fd.ftLastWriteTime.dwLowDateTime;
            entries[count].t.HighPart = fd.ftLastWriteTime.dwHighDateTime;
            count++;
        }
    } while (FindNextFileW(fh, &fd));
    FindClose(fh);
    // Удаляем все, кроме 5 newest (простой выбор максимума).
    while (count > 5) {
        int oldest = 0;
        for (int i = 1; i < count; i++) {
            if (entries[i].t.QuadPart < entries[oldest].t.QuadPart) oldest = i;
        }
        wchar_t full[MAX_PATH] = {};
        _snwprintf_s(full, _countof(full), _TRUNCATE, L"%s\\%s", dir, entries[oldest].name);
        DeleteFileW(full);
        entries[oldest] = entries[count - 1];
        count--;
    }
}

// Установить handler. tag — "host"/"cli" для имени файла. Вызывать первым делом в main.
inline void InstallCrashHandler(const wchar_t* tag)
{
    detail::CrashTagStorage() = tag;
    PruneOldDumps();
    SetUnhandledExceptionFilter(detail::CrashFilter);
}

} // namespace vcam
