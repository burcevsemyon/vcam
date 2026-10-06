#pragma once

// Runtime-метрики продьюсера как данные (backlog observability.md P1.2).
// Хост (долгоживущий продьюсер) периодически пишет снапшот; `status --json` читает.
// Формат (JSON, одна строка, UTF-8 без BOM, только числа):
//   {"switches":N,"fallbacks":N,"frameMinUs":N,"frameMaxUs":N,"frameAvgUs":N,
//    "frameCount":N,"recFrames":N,"recDropped":N}
// Запись атомарна (tmp + rename), чтение best-effort (нет файла/битый → false).

#include <windows.h>
#include <atlbase.h>

#include <cstdint>
#include <cstdio>
#include <string>

namespace vcam {

struct RuntimeCounters {
    int64_t switches = 0;
    int64_t fallbacks = 0;
    int64_t frameMinUs = 0;
    int64_t frameMaxUs = 0;
    int64_t frameAvgUs = 0;
    int64_t frameCount = 0;
    uint64_t recFrames = 0;
    uint64_t recDropped = 0;
};

// %APPDATA%\VCam\runtime_counters.json (пусто, если APPDATA недоступен).
inline std::wstring RuntimeCountersPath()
{
    wchar_t appdata[MAX_PATH] = {};
    DWORD n = GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return std::wstring();
    return std::wstring(appdata) + L"\\VCam\\runtime_counters.json";
}

// Пишет снапшот runtime-метрик. Для хоста (периодический снапшот).
inline bool WriteRuntimeCounters(const RuntimeCounters& c)
{
    std::wstring path = RuntimeCountersPath();
    if (path.empty()) return false;
    char buf[256];
    int n = snprintf(buf, sizeof(buf),
        "{\"switches\":%lld,\"fallbacks\":%lld,"
        "\"frameMinUs\":%lld,\"frameMaxUs\":%lld,\"frameAvgUs\":%lld,"
        "\"frameCount\":%lld,\"recFrames\":%llu,\"recDropped\":%llu}\n",
        (long long)c.switches, (long long)c.fallbacks,
        (long long)c.frameMinUs, (long long)c.frameMaxUs, (long long)c.frameAvgUs,
        (long long)c.frameCount,
        (unsigned long long)c.recFrames, (unsigned long long)c.recDropped);
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

// Читает runtime-метрики. Для `status --json`. false = нет файла/битый.
inline bool ReadRuntimeCounters(RuntimeCounters& out)
{
    out = RuntimeCounters{};
    std::wstring path = RuntimeCountersPath();
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
    long long sw = 0, fb = 0, fmin = 0, fmax = 0, favg = 0, fcnt = 0;
    unsigned long long rf = 0, rd = 0;
    int n = sscanf_s(buf,
        "{\"switches\":%lld,\"fallbacks\":%lld,"
        "\"frameMinUs\":%lld,\"frameMaxUs\":%lld,\"frameAvgUs\":%lld,"
        "\"frameCount\":%lld,\"recFrames\":%llu,\"recDropped\":%llu}",
        &sw, &fb, &fmin, &fmax, &favg, &fcnt, &rf, &rd);
    if (n != 8) return false;
    out.switches = (int64_t)sw;
    out.fallbacks = (int64_t)fb;
    out.frameMinUs = (int64_t)fmin;
    out.frameMaxUs = (int64_t)fmax;
    out.frameAvgUs = (int64_t)favg;
    out.frameCount = (int64_t)fcnt;
    out.recFrames = (uint64_t)rf;
    out.recDropped = (uint64_t)rd;
    return true;
}

} // namespace vcam
