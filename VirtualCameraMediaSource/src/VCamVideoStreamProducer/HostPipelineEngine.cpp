#include "HostPipelineEngine.h"

#include "Settings.h"
#include "SharedMemoryContract.h"
#include <atlbase.h>

#include <windows.h>
#include <shlobj.h>

namespace {

std::wstring RecordStatePath()
{
    wchar_t appdata[MAX_PATH] = {};
    DWORD n = GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return {};
    return std::wstring(appdata) + L"\\VCam\\record_state.json";
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

void WriteRecordState(const std::wstring& path)
{
    std::wstring sp = RecordStatePath();
    if (sp.empty()) return;
    size_t slash = sp.find_last_of(L"\\/");
    if (slash != std::wstring::npos) CreateDirectoryW(sp.substr(0, slash).c_str(), nullptr);
    std::string utf8 = "{\"recording\":true,\"path\":\"";
    for (wchar_t c : path) utf8 += (c < 0x80) ? (char)c : '?';
    utf8 += "\",\"started\":" + std::to_string((long long)time(nullptr)) + "}\n";
    HANDLE raw = CreateFileW(sp.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                             CREATE_ALWAYS, 0, nullptr);
    if (raw == INVALID_HANDLE_VALUE) return;
    ATL::CHandle h(raw);
    DWORD written = 0;
    WriteFile(h, utf8.data(), (DWORD)utf8.size(), &written, nullptr);
}

void ClearRecordState()
{
    std::wstring sp = RecordStatePath();
    if (!sp.empty()) DeleteFileW(sp.c_str());
}

struct SettingsFileGuard {
    SettingsFileGuard() : guard(m_settingsCs) {}
    static ATL::CComAutoCriticalSection m_settingsCs;
private:
    ATL::CComCritSecLock<ATL::CComAutoCriticalSection> guard;
};

ATL::CComAutoCriticalSection SettingsFileGuard::m_settingsCs;

} // namespace

HostPipelineEngine::HostPipelineEngine() = default;

HostPipelineEngine::~HostPipelineEngine()
{
    if (m_rec.IsOpen())
        StopRecording(L"destructor");
}

void HostPipelineEngine::Log(const std::wstring& msg)
{
    HostLog(L"[host] " + msg);
}

void HostPipelineEngine::PostProcessFrame(uint8_t* bgrx, int stride, uint32_t w, uint32_t h)
{
    (void)bgrx;
    (void)stride;
    (void)w;
    (void)h;
}

bool HostPipelineEngine::StartRecording(const std::wstring& requested)
{
    if (m_rec.IsOpen()) {
        Log(L"record start ignored: already recording (" + m_rec.Path() + L")");
        return false;
    }
    std::wstring settingsPath = DefaultSettingsPath();
    Settings cur;
    bool haveSettings = !settingsPath.empty() && cur.Load(settingsPath);
    std::wstring path = requested;
    if (path.empty() && haveSettings) path = cur.record.path;
    if (path.empty()) path = DefaultRecordPath();
    if (path.empty()) {
        Log(L"record start: no path resolved (settings + default both empty)");
        return false;
    }
    std::wstring err;
    if (!m_rec.Start(path, err)) {
        Log(L"record start failed: " + path + L" (" + err + L")");
        return false;
    }
    m_recErrLogged = false;
    m_recLastDropped = 0;
    WriteRecordState(path);
    {
        SettingsFileGuard fsg;
        Settings fresh;
        if (!settingsPath.empty() && fresh.Load(settingsPath) && fresh.record.path != path) {
            fresh.record.path = path;
            if (fresh.Save(settingsPath))
                Log(L"record: default path saved to settings");
            else
                Log(L"record: settings save failed (recording continues)");
        }
    }
    Log(L"record started: " + path);
    return true;
}

void HostPipelineEngine::StopRecording(const std::wstring& reason)
{
    if (!m_rec.IsOpen()) return;
    std::wstring path = m_rec.Path();
    uint64_t frames = m_rec.FramesWritten();
    uint64_t dropped = m_rec.FramesDropped();
    ULONGLONG ms = GetTickCount64() - m_rec.StartTickMs();
    m_rec.Stop();
    ClearRecordState();
    std::wstring err = m_rec.LastError();
    Log(L"record stopped (" + reason + L"): " + path + L" (frames=" +
        std::to_wstring(frames) + L", " + std::to_wstring(ms / 1000.0) + L"s, dropped=" +
        std::to_wstring(dropped) + (err.empty() ? L"" : L", " + err) + L")");
}

void HostPipelineEngine::RecordEtherFrame()
{
    if (!m_rec.IsOpen()) return;
    if (Buf().empty() || FrameW() == 0 || FrameH() == 0) return;
    if (!m_rec.WriteFrameNative(Buf().data(), (int)(FrameW() * 4), FrameW(), FrameH())) {
        if (!m_recErrLogged) {
            m_recErrLogged = true;
            Log(L"record write failed: " + m_rec.LastError() + L" - recording stopped, эфир продолжается (fail-open)");
        }
        StopRecording(L"ошибка записи");
        return;
    }
    if (m_rec.FramesDropped() != m_recLastDropped) {
        m_recLastDropped = m_rec.FramesDropped();
        Log(L"record: encoder lags, dropped=" + std::to_wstring(m_recLastDropped) + L" (эфир не ждёт)");
    }
}
