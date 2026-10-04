#include <windows.h>

#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <ctime>

#include "HostGlobals.h"
#include "HostLogging.h"

std::wstring LogFilePath()
{
    wchar_t base[MAX_PATH] = {};
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return {};
    std::wstring dir = std::wstring(base) + L"\\VCam";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"\\host.log";
}

void RotateLog(const std::wstring& path)
{
    WIN32_FILE_ATTRIBUTE_DATA a = {};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a)) return;
    ULARGE_INTEGER sz = {};
    sz.LowPart = a.nFileSizeLow;
    sz.HighPart = a.nFileSizeHigh;
    if (sz.QuadPart <= 1024ull * 1024ull) return;
    std::wstring old = path + L".old";
    DeleteFileW(old.c_str());
    MoveFileW(path.c_str(), old.c_str());
}

void Log(const wchar_t* fmt, ...)
{
    wchar_t msg[768];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf_s(msg, _countof(msg), _TRUNCATE, fmt, ap);
    va_end(ap);

    SYSTEMTIME st = {};
    GetLocalTime(&st);
    wchar_t line[860];
    swprintf_s(line, L"[%02u:%02u:%02u.%03u] %s\r\n", st.wHour, st.wMinute, st.wSecond,
               st.wMilliseconds, msg);

    fputws(line, stdout);
    fflush(stdout);
    if (static_cast<HANDLE>(g_logFile) != INVALID_HANDLE_VALUE &&
        static_cast<HANDLE>(g_logFile) != nullptr) {
        char utf8[2200];
        int n = WideCharToMultiByte(CP_UTF8, 0, line, -1, utf8, sizeof(utf8), nullptr, nullptr);
        if (n > 2) {
            DWORD written = 0;
            WriteFile(static_cast<HANDLE>(g_logFile), utf8, (DWORD)(n - 1), &written,
                      nullptr);
        }
    }
}

void InitLogging()
{
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!h || h == INVALID_HANDLE_VALUE) {
        if (AttachConsole(ATTACH_PARENT_PROCESS)) {
            FILE* f = nullptr;
            freopen_s(&f, "CONOUT$", "w", stdout);
        }
    }
    std::wstring path = LogFilePath();
    if (!path.empty()) {
        RotateLog(path);
        g_logFile.Attach(CreateFileW(path.c_str(), FILE_APPEND_DATA,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                      OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (static_cast<HANDLE>(g_logFile) == INVALID_HANDLE_VALUE) {
            g_logFile.Detach();
        }
    }
}

void HostLog(const std::wstring& msg)
{
    Log(L"%s", msg.c_str());
}
