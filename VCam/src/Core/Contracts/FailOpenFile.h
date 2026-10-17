#pragma once

// Персистентность счётчиков fail-open (backlog observability.md P0.3).
// Хост (долгоживущий продьюсер) периодически пишет свои счётчики в файл;
// `VCamProducerCli status` читает и показывает их одним вызовом.
// Формат (JSON, одна строка, UTF-8 без BOM):
//   {"settingsBrokenJson":N,"hotkeyBusy":N,"recordStartFailed":N,
//    "sourceOpenFailed":N,"staleTransientRemoved":N,"controlUnavailable":N}
// Порядок значений = порядок `FailOpen` (FailOpenCounters.h). Запись атомарна
// (tmp + rename), чтение best-effort (нет файла/битый → false).

#include <windows.h>
#include <atlbase.h>

#include <cstdint>
#include <cstdio>
#include <string>

#include "FailOpenCounters.h"

namespace vcam {

// %APPDATA%\VCam\failopen_counters.json (пусто, если APPDATA недоступен).
inline std::wstring FailOpenCountersPath()
{
    wchar_t appdata[MAX_PATH] = {};
    DWORD n = GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return std::wstring();
    return std::wstring(appdata) + L"\\VCam\\failopen_counters.json";
}

// Пишет текущие счётчики (GetFailOpen) в файл. Для хоста (периодический снапшот).
inline bool WriteFailOpenCounters()
{
    std::wstring path = FailOpenCountersPath();
    if (path.empty()) return false;
    char buf[256];
    int n = snprintf(buf, sizeof(buf),
        "{\"settingsBrokenJson\":%lld,\"hotkeyBusy\":%lld,"
        "\"recordStartFailed\":%lld,\"sourceOpenFailed\":%lld,"
        "\"staleTransientRemoved\":%lld,\"controlUnavailable\":%lld}\n",
        (long long)GetFailOpen(FailOpen::SettingsBrokenJson),
        (long long)GetFailOpen(FailOpen::HotkeyBusy),
        (long long)GetFailOpen(FailOpen::RecordStartFailed),
        (long long)GetFailOpen(FailOpen::SourceOpenFailed),
        (long long)GetFailOpen(FailOpen::StaleTransientRemoved),
        (long long)GetFailOpen(FailOpen::ControlUnavailable));
    if (n <= 0 || n >= (int)sizeof(buf)) return false;
    std::wstring tmp = path + L".tmp";
    HANDLE raw = CreateFileW(tmp.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                              CREATE_ALWAYS, 0, nullptr);
    if (raw == INVALID_HANDLE_VALUE) return false;
    {
        ATL::CHandle h(raw);
        DWORD written = 0;
        if (!WriteFile(h, buf, (DWORD)n, &written, nullptr) || written != (DWORD)n)
            return false;
    }
    return MoveFileWithProgressW(tmp.c_str(), path.c_str(), nullptr, nullptr,
                                  MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}

// Читает счётчики в values[6] (порядок FailOpen). Для `status`. false = нет/битый.
inline bool ReadFailOpenCounters(int64_t values[6])
{
    std::wstring path = FailOpenCountersPath();
    if (path.empty()) return false;
    HANDLE raw = CreateFileW(path.c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, 0, nullptr);
    if (raw == INVALID_HANDLE_VALUE) return false;
    ATL::CHandle h(raw);
    char buf[256] = {};
    DWORD read = 0;
    if (!ReadFile(h, buf, (DWORD)sizeof(buf) - 1, &read, nullptr) || read == 0)
        return false;
    buf[read] = '\0';
    long long v[6] = {};
    int n = sscanf_s(buf,
        "{\"settingsBrokenJson\":%lld,\"hotkeyBusy\":%lld,"
        "\"recordStartFailed\":%lld,\"sourceOpenFailed\":%lld,"
        "\"staleTransientRemoved\":%lld,\"controlUnavailable\":%lld}",
        &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]);
    if (n != 6) return false;
    for (int i = 0; i < 6; i++) values[i] = (int64_t)v[i];
    return true;
}

} // namespace vcam
