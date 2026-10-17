#pragma once

#include <string>

void Log(const wchar_t* fmt, ...);
void LogDebug(const wchar_t* fmt, ...);
void HostLog(const std::wstring& msg);
void InitLogging();
