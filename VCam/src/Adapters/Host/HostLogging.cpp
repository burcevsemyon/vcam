#include <windows.h>

#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <mutex>
#include <string>

#include "HostGlobals.h"
#include "HostLogging.h"
#include "LogFormat.h"
#include "LogRotate.h"

std::wstring LogFilePath()
{
    wchar_t base[MAX_PATH] = {};
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return {};
    std::wstring dir = std::wstring(base) + L"\\VCam";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"\\host.log";
}

// --- runtime-ротация host.log (P0.4): cap + ротация в .old ---
static std::wstring s_logFilePath;
static uint64_t s_logBytes = 0;
static std::mutex s_logCs;

void ReopenHostLog()
{
    if (s_logFilePath.empty()) return;
    g_logFile.Attach(CreateFileW(s_logFilePath.c_str(), FILE_APPEND_DATA,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                  OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (static_cast<HANDLE>(g_logFile) == INVALID_HANDLE_VALUE) g_logFile.Detach();
}

void Log(const wchar_t* fmt, ...)
{
    wchar_t msg[768];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf_s(msg, _countof(msg), _TRUNCATE, fmt, ap);
    va_end(ap);

    // Единый формат строки (Common/LogFormat.h): [дата время] [уровень] [host] msg
    wchar_t line[960];
    int plen = vcam::FormatLogPrefix(line, 64, vcam::LogLevel::Info, L"host");
    if (plen < 0) plen = 0;
    _snwprintf_s(line + plen, (int)_countof(line) - plen, _TRUNCATE, L"%s\r\n", msg);

    fputws(line, stdout);
    fflush(stdout);

    std::lock_guard<std::mutex> guard(s_logCs);
    if ((static_cast<HANDLE>(g_logFile) == INVALID_HANDLE_VALUE ||
         static_cast<HANDLE>(g_logFile) == nullptr) && !s_logFilePath.empty()) {
        ReopenHostLog(); // восстановить хэндл после сбоя/ротации
    }
    if (static_cast<HANDLE>(g_logFile) != INVALID_HANDLE_VALUE &&
        static_cast<HANDLE>(g_logFile) != nullptr) {
        // Ротация по cap (P0.4): переполнен -> закрыть, ротировать в .old, переоткрыть.
        if (s_logBytes >= vcam::kHostLogCapBytes) {
            g_logFile.Close();
            vcam::RotateLogIfOverCap(s_logFilePath, vcam::kHostLogCapBytes);
            ReopenHostLog();
            s_logBytes = 0;
        }
        char utf8[3200];
        int n = WideCharToMultiByte(CP_UTF8, 0, line, -1, utf8, sizeof(utf8), nullptr, nullptr);
        if (n > 2) {
            DWORD written = 0;
            WriteFile(static_cast<HANDLE>(g_logFile), utf8, (DWORD)(n - 1), &written, nullptr);
            s_logBytes += (uint64_t)(n - 1);
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
        s_logFilePath = path;
        vcam::RotateLogIfOverCap(path, vcam::kHostLogCapBytes); // ротация на старте
        ReopenHostLog();
    }
}

void HostLog(const std::wstring& msg)
{
    Log(L"%s", msg.c_str());
}

// Debug-строка (P1.1): только при VCAM_DEBUG=1, иначе no-op без форматирования.
void LogDebug(const wchar_t* fmt, ...)
{
    if (!vcam::IsDebugEnabled()) return;
    wchar_t msg[768];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf_s(msg, _countof(msg), _TRUNCATE, fmt, ap);
    va_end(ap);

    wchar_t line[960];
    int plen = vcam::FormatLogPrefix(line, 64, vcam::LogLevel::Debug, L"host");
    if (plen < 0) plen = 0;
    _snwprintf_s(line + plen, (int)_countof(line) - plen, _TRUNCATE, L"%s\r\n", msg);

    fputws(line, stdout);
    fflush(stdout);

    std::lock_guard<std::mutex> guard(s_logCs);
    if ((static_cast<HANDLE>(g_logFile) == INVALID_HANDLE_VALUE ||
         static_cast<HANDLE>(g_logFile) == nullptr) && !s_logFilePath.empty()) {
        ReopenHostLog();
    }
    if (static_cast<HANDLE>(g_logFile) != INVALID_HANDLE_VALUE &&
        static_cast<HANDLE>(g_logFile) != nullptr) {
        if (s_logBytes >= vcam::kHostLogCapBytes) {
            g_logFile.Close();
            vcam::RotateLogIfOverCap(s_logFilePath, vcam::kHostLogCapBytes);
            ReopenHostLog();
            s_logBytes = 0;
        }
        char utf8[3200];
        int n = WideCharToMultiByte(CP_UTF8, 0, line, -1, utf8, sizeof(utf8), nullptr, nullptr);
        if (n > 2) {
            DWORD written = 0;
            WriteFile(static_cast<HANDLE>(g_logFile), utf8, (DWORD)(n - 1), &written, nullptr);
            s_logBytes += (uint64_t)(n - 1);
        }
    }
}
