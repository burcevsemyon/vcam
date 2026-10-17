#include <windows.h>
#include <shlobj.h>
#include <atlbase.h>

#include <cctype>
#include <cstdio>
#include <ctime>

#include <string>
#include <vector>

#include "HostGlobals.h"
#include "HostLogging.h"
#include "HostRecording.h"

std::wstring RecordStatePath()
{
    wchar_t appdata[MAX_PATH] = {};
    DWORD n = GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return {};
    return std::wstring(appdata) + L"\\VCam\\record_state.json";
}

std::wstring RecordCommandPath()
{
    wchar_t appdata[MAX_PATH] = {};
    DWORD n = GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return {};
    return std::wstring(appdata) + L"\\VCam\\record_command.json";
}

std::wstring DefaultRecordPath()
{
    std::wstring videos;
    wchar_t* known = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Videos, 0, nullptr, &known)) && known) {
        videos = known;
        CoTaskMemFree(known);
    }
    if (videos.empty()) {
        wchar_t up[MAX_PATH] = {};
        DWORD n = GetEnvironmentVariableW(L"USERPROFILE", up, MAX_PATH);
        if (n == 0 || n >= MAX_PATH) return {};
        videos = std::wstring(up) + L"\\Videos";
    }
    SYSTEMTIME st = {};
    GetLocalTime(&st);
    wchar_t name[64];
    swprintf_s(name, L"VCam_%04u%02u%02u_%02u%02u%02u.mp4",
               st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return videos + L"\\" + name;
}

std::string EscapeJsonUtf8(const std::wstring& w)
{
    std::string out;
    if (w.empty()) return out;
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0,
                                nullptr, nullptr);
    if (n <= 0) return out;
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    for (char c : s) {
        switch (c) {
        case '\\': out += "\\\\"; break;
        case '"': out += "\\\""; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if ((unsigned char)c < 0x20) {
                char b[8];
                snprintf(b, sizeof(b), "\\u%04x", (unsigned char)c);
                out += b;
            } else {
                out += c;
            }
        }
    }
    return out;
}

void WriteRecordState(const std::wstring& path)
{
    std::wstring sp = RecordStatePath();
    if (sp.empty()) return;
    size_t slash = sp.find_last_of(L"\\/");
    if (slash != std::wstring::npos) CreateDirectoryW(sp.substr(0, slash).c_str(), nullptr);
    std::string utf8 = "{\"recording\":true,\"path\":\"";
    utf8 += EscapeJsonUtf8(path);
    utf8 += "\",\"started\":";
    utf8 += std::to_string((long long)time(nullptr));
    utf8 += "}\n";
    HANDLE raw = CreateFileW(sp.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                             CREATE_ALWAYS, 0, nullptr);
    if (raw == INVALID_HANDLE_VALUE) {
        Log(L"record state write failed: %lu", GetLastError());
        return;
    }
    ATL::CHandle h(raw);
    DWORD written = 0;
    WriteFile(h, utf8.data(), (DWORD)utf8.size(), &written, nullptr);
}

void ClearRecordState()
{
    std::wstring sp = RecordStatePath();
    if (!sp.empty()) DeleteFileW(sp.c_str());
}

static void SkipScanWs(const std::string& json, size_t& p)
{
    while (p < json.size() && (json[p] == ' ' || json[p] == '\t' ||
                               json[p] == '\r' || json[p] == '\n'))
        p++;
}

static size_t ScanKeyPos(const std::string& json, const char* key, size_t from = 0)
{
    std::string k = std::string("\"") + key + "\"";
    size_t p = json.find(k, from);
    while (p != std::string::npos) {
        size_t c = p + k.size();
        SkipScanWs(json, c);
        if (c < json.size() && json[c] == ':') return p;
        p = json.find(k, p + 1);
    }
    return std::string::npos;
}

bool ScanJsonString(const std::string& json, const char* key, std::wstring& value)
{
    size_t kp = ScanKeyPos(json, key);
    if (kp == std::string::npos) return false;
    size_t p = json.find(':', kp);
    if (p == std::string::npos) return false;
    p++;
    SkipScanWs(json, p);
    if (p >= json.size() || json[p] != '"') return false;
    std::string val;
    for (p++; p < json.size() && json[p] != '"'; p++) {
        if (json[p] == '\\' && p + 1 < json.size()) {
            char c = json[++p];
            switch (c) {
            case '"': val += '"'; break;
            case '\\': val += '\\'; break;
            case '/': val += '/'; break;
            case 'b': val += '\b'; break;
            case 'f': val += '\f'; break;
            case 'n': val += '\n'; break;
            case 'r': val += '\r'; break;
            case 't': val += '\t'; break;
            case 'u':
                if (p + 4 < json.size()) p += 4;
                val += '?';
                break;
            default: val += c; break;
            }
        } else {
            val += json[p];
        }
    }
    if (p >= json.size()) return false;
    if (val.empty()) { value.clear(); return true; }
    int m = MultiByteToWideChar(CP_UTF8, 0, val.c_str(), (int)val.size(), nullptr, 0);
    if (m <= 0) return false;
    value.resize((size_t)m);
    MultiByteToWideChar(CP_UTF8, 0, val.c_str(), (int)val.size(), &value[0], m);
    return true;
}

bool ScanJsonInt(const std::string& json, const char* key, long long& value)
{
    size_t kp = ScanKeyPos(json, key);
    if (kp == std::string::npos) return false;
    size_t p = json.find(':', kp);
    if (p == std::string::npos) return false;
    p++;
    SkipScanWs(json, p);
    if (p >= json.size() || json[p] < '0' || json[p] > '9') return false;
    long long v = 0;
    for (; p < json.size() && json[p] >= '0' && json[p] <= '9'; p++) {
        v = v * 10 + (json[p] - '0');
        if (v > 99999999999LL) break;
    }
    value = v;
    return true;
}

bool ReadSmallFile(const std::wstring& path, std::string& out)
{
    HANDLE raw = CreateFileW(path.c_str(), GENERIC_READ,
                             FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                             nullptr, OPEN_EXISTING, 0, nullptr);
    if (raw == INVALID_HANDLE_VALUE) return false;
    ATL::CHandle h(raw);
    LARGE_INTEGER sz = {};
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart <= 0 || sz.QuadPart > 65536) return false;
    out.resize((size_t)sz.QuadPart);
    DWORD read = 0;
    return ReadFile(h, &out[0], (DWORD)out.size(), &read, nullptr) && read == out.size();
}

bool TryReadRecordState(std::wstring& path, long long& started)
{
    path.clear();
    started = 0;
    std::wstring sp = RecordStatePath();
    if (sp.empty()) return false;
    std::string json;
    if (!ReadSmallFile(sp, json)) return false;
    size_t kp = ScanKeyPos(json, "recording");
    if (kp == std::string::npos) return false;
    size_t p = json.find(':', kp);
    if (p == std::string::npos) return false;
    p++;
    SkipScanWs(json, p);
    bool rec;
    if (json.compare(p, 4, "true") == 0 &&
        (p + 4 >= json.size() || (!isalnum((unsigned char)json[p + 4]) && json[p + 4] != '_')))
        rec = true;
    else if (json.compare(p, 5, "false") == 0 &&
             (p + 5 >= json.size() || (!isalnum((unsigned char)json[p + 5]) && json[p + 5] != '_')))
        rec = false;
    else
        return false;
    if (!rec) return false;
    ScanJsonString(json, "path", path);
    ScanJsonInt(json, "started", started);
    return true;
}

bool WriteRecordCommand(const std::wstring& cmd, const std::wstring& path)
{
    std::wstring cp = RecordCommandPath();
    if (cp.empty()) return false;
    size_t slash = cp.find_last_of(L"\\/");
    if (slash != std::wstring::npos) CreateDirectoryW(cp.substr(0, slash).c_str(), nullptr);
    std::string utf8 = "{\"cmd\":\"";
    for (wchar_t c : cmd) utf8 += (c < 0x80) ? (char)c : '?';
    utf8 += "\",\"path\":\"" + EscapeJsonUtf8(path) + "\"}\n";
    std::wstring tmp = cp + L".tmp";
    HANDLE raw = CreateFileW(tmp.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                             CREATE_ALWAYS, 0, nullptr);
    if (raw == INVALID_HANDLE_VALUE) return false;
    {
        ATL::CHandle h(raw);
        DWORD written = 0;
        if (!WriteFile(h, utf8.data(), (DWORD)utf8.size(), &written, nullptr) ||
            written != utf8.size())
            return false;
    }
    return MoveFileWithProgressW(tmp.c_str(), cp.c_str(), nullptr, nullptr,
                                 MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}

bool ConsumeRecordCommand(std::wstring& cmd, std::wstring& path)
{
    cmd.clear();
    path.clear();
    std::wstring cp = RecordCommandPath();
    if (cp.empty()) return false;
    std::wstring proc = cp + L".processing";
    if (!MoveFileWithProgressW(cp.c_str(), proc.c_str(), nullptr, nullptr,
                               MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        return false;
    std::string json;
    if (!ReadSmallFile(proc, json)) {
        DeleteFileW(proc.c_str());
        return false;
    }
    DeleteFileW(proc.c_str());
    if (!ScanJsonString(json, "cmd", cmd)) return false;
    ScanJsonString(json, "path", path);
    return !cmd.empty();
}
