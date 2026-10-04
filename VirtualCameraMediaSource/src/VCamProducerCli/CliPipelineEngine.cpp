#include "CliPipelineEngine.h"

#include <windows.h>

void CliLog(const std::wstring& msg)
{
    std::wstring line = msg + L'\n';
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!h || h == INVALID_HANDLE_VALUE) return;
    DWORD mode = 0;
    if (GetConsoleMode(h, &mode)) {
        DWORD n = 0;
        WriteConsoleW(h, line.c_str(), (DWORD)line.size(), &n, nullptr);
        return;
    }
    int bytes = WideCharToMultiByte(CP_UTF8, 0, line.c_str(), (int)line.size(),
                                    nullptr, 0, nullptr, nullptr);
    if (bytes <= 0) return;
    std::string s((size_t)bytes, '\0');
    WideCharToMultiByte(CP_UTF8, 0, line.c_str(), (int)line.size(), &s[0], bytes,
                        nullptr, nullptr);
    DWORD n = 0;
    WriteFile(h, s.data(), (DWORD)s.size(), &n, nullptr);
}

CliPipelineEngine::CliPipelineEngine() = default;

CliPipelineEngine::~CliPipelineEngine() = default;

void CliPipelineEngine::Log(const std::wstring& msg)
{
    CliLog(L"[cli] " + msg);
}
