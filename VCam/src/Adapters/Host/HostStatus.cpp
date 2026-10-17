#include <windows.h>
#include <atlbase.h>

#include <ctime>
#include <string>

#include "HostGlobals.h"
#include "HostLogging.h"
#include "HostRecording.h"
#include "HostHotkey.h"
#include "HostStatus.h"
#include "FailOpenCounters.h"

std::wstring TargetLabel(const SourceConfig& cfg)
{
    if (!cfg.camName.empty()) return cfg.camName;
    if (cfg.path.empty()) return cfg.type == L"camera" ? std::wstring(L"(камера не выбрана)")
                                                       : std::wstring(L"(путь не задан)");
    return cfg.path;
}

std::wstring ActiveStatus(const SourceConfig& cfg, const std::wstring& quality)
{
    return L"Источник: " + cfg.type + L" — " + TargetLabel(cfg) + L" (" + quality + L")";
}

std::wstring FallbackStatus(const std::wstring& reason)
{
    return L"Нет сигнала (" + reason + L")";
}

void SetActiveStatus(HostPipelineEngine& e)
{
    if (e.IsWriterOpen()) SetStatus(ActiveStatus(e.Target(), e.Quality()));
    else SetStatus(FallbackStatus(e.WriterErr()));
}

void CleanupStaleRecordFiles()
{
    std::wstring cp = RecordCommandPath();
    if (!cp.empty() && GetFileAttributesW(cp.c_str()) != INVALID_FILE_ATTRIBUTES) {
        DeleteFileW(cp.c_str());
        vcam::IncFailOpen(vcam::FailOpen::StaleTransientRemoved);
        Log(L"record: stale command removed (not resuming after restart)");
    }
    if (!cp.empty()) {
        std::wstring proc = cp + L".processing";
        if (GetFileAttributesW(proc.c_str()) != INVALID_FILE_ATTRIBUTES) {
            DeleteFileW(proc.c_str());
            vcam::IncFailOpen(vcam::FailOpen::StaleTransientRemoved);
            Log(L"record: stale command processing removed (not resuming after restart)");
        }
    }
    std::wstring curPath;
    long long curStarted = 0;
    if (TryReadRecordState(curPath, curStarted)) {
        ClearRecordState();
        vcam::IncFailOpen(vcam::FailOpen::StaleTransientRemoved);
        Log(L"record: stale state removed (was: %s)", curPath.c_str());
    }
}

// --- P2.2: crash-маркер + последний шаг/фаза (переживают рестарт) ---
namespace {
std::wstring RunStatePath()
{
    wchar_t appdata[MAX_PATH] = {};
    DWORD n = GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return {};
    return std::wstring(appdata) + L"\\VCam\\host_runstate.json";
}

DWORD s_runPid = 0;
long long s_runStarted = 0;
std::wstring s_lastStep;

bool WriteRunState()
{
    std::wstring path = RunStatePath();
    if (path.empty() || s_runPid == 0) return false;
    wchar_t buf[512];
    int n = swprintf_s(buf, L"{\"pid\":%u,\"started\":%lld,\"cleanShutdown\":false,\"lastStep\":\"%s\"}\n",
        (unsigned)s_runPid, s_runStarted, s_lastStep.c_str());
    if (n <= 0) return false;
    char utf8[1024] = {};
    int m = WideCharToMultiByte(CP_UTF8, 0, buf, -1, utf8, sizeof(utf8), nullptr, nullptr);
    if (m <= 1) return false;
    std::wstring tmp = path + L".tmp";
    HANDLE raw = CreateFileW(tmp.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                              CREATE_ALWAYS, 0, nullptr);
    if (raw == INVALID_HANDLE_VALUE) return false;
    {
        ATL::CHandle h(raw);
        DWORD written = 0;
        if (!WriteFile(h, utf8, (DWORD)(m - 1), &written, nullptr) || written != (DWORD)(m - 1))
            return false;
    }
    return MoveFileWithProgressW(tmp.c_str(), path.c_str(), nullptr, nullptr,
                                  MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}
} // namespace

void CheckPreviousRunCrash()
{
    s_runPid = GetCurrentProcessId();
    s_runStarted = (long long)time(nullptr);
    s_lastStep = L"starting";
    std::wstring path = RunStatePath();
    std::string json;
    if (!path.empty() && ReadSmallFile(path, json) && !json.empty()) {
        long long pid = 0, started = 0;
        std::wstring lastStep;
        ScanJsonInt(json, "pid", pid);
        ScanJsonInt(json, "started", started);
        ScanJsonString(json, "lastStep", lastStep);
        Log(L"previous run did not shut down cleanly (pid=%lld started=%lld last step: %s) - possible crash/kill",
            pid, started, lastStep.empty() ? L"(unknown)" : lastStep.c_str());
    }
    WriteRunState(); // свежий "starting" (перезаписывает stale)
}

void UpdateRunStep(const std::wstring& step)
{
    if (step == s_lastStep) return;
    s_lastStep = step;
    WriteRunState();
}

void ClearRunState()
{
    std::wstring path = RunStatePath();
    if (!path.empty()) DeleteFileW(path.c_str());
    s_runPid = 0;
    s_lastStep.clear();
}

void ApplySettingsDiff(HostPipelineEngine& e, HotkeySection& curHotkey,
                       RecordHotkeySection& curRecHotkey,
                       VideoHotkeySection& curVideoHotkey,
                       SourceSwitchHotkeySection& curSrcStaticHotkey,
                       SourceSwitchHotkeySection& curSrcVideoHotkey,
                       SourceSwitchHotkeySection& curSrcCameraHotkey)
{
    Settings s;
    g_watcher.Current(s);
    if (s.hotkey != curHotkey) {
        curHotkey = s.hotkey;
        {
            ATL::CComCritSecLock<ATL::CComAutoCriticalSection> guard(g_hotkeyCs);
            g_hotkey = s.hotkey;
        }
        PostMessageW(g_hwnd, WM_REAPPLY_HOTKEY, 0, 0);
    }
    if (s.recordHotkey != curRecHotkey) {
        curRecHotkey = s.recordHotkey;
        {
            ATL::CComCritSecLock<ATL::CComAutoCriticalSection> guard(g_hotkeyCs);
            g_recHotkey = s.recordHotkey;
        }
        PostMessageW(g_hwnd, WM_REAPPLY_HOTKEY, 0, 0);
    }
    if (s.videoHotkey != curVideoHotkey) {
        curVideoHotkey = s.videoHotkey;
        {
            ATL::CComCritSecLock<ATL::CComAutoCriticalSection> guard(g_hotkeyCs);
            g_videoHotkey = s.videoHotkey;
        }
        PostMessageW(g_hwnd, WM_REAPPLY_HOTKEY, 0, 0);
    }
    // Хоткеи переключения источника: смена — только перерегистрация.
    {
        struct {
            SourceSwitchHotkeySection* cur;
            SourceSwitchHotkeySection* glob;
            const SourceSwitchHotkeySection* want;
        } srcHks[] = {
            { &curSrcStaticHotkey, &g_sourceStaticHotkey, &s.sourceStaticHotkey },
            { &curSrcVideoHotkey, &g_sourceVideoHotkey, &s.sourceVideoHotkey },
            { &curSrcCameraHotkey, &g_sourceCameraHotkey, &s.sourceCameraHotkey },
        };
        for (auto& sh : srcHks) {
            if (*sh.want == *sh.cur) continue;
            *sh.cur = *sh.want;
            {
                ATL::CComCritSecLock<ATL::CComAutoCriticalSection> guard(g_hotkeyCs);
                *sh.glob = *sh.want;
            }
            PostMessageW(g_hwnd, WM_REAPPLY_HOTKEY, 0, 0);
        }
    }
    SourceConfig want = ToSourceConfig(s);
    bool borrowed = false;
    {
        ATL::CComCritSecLock<ATL::CComAutoCriticalSection> guard(g_hotkeyCs);
        borrowed = g_hotkeyBorrowed;
    }
    if (borrowed && want.type != L"video") {
        {
            ATL::CComCritSecLock<ATL::CComAutoCriticalSection> guard(g_hotkeyCs);
            g_hotkeyBorrowed = false;
            g_hotkeyBorrowTickMs = 0;
        }
        ClearHotkeyState();
        Log(L"hotkey: manual switch away from borrowed video - borrow dropped");
        borrowed = false;
    }
    if (borrowed && want.type == L"video") want.playOnce = true;
    // P1.4: что применено — на старте и каждом перечитывании настроек (Info).
    Log(L"config applied: type=%s path=%s quality=%s record=%s",
        want.type.c_str(), TargetLabel(want).c_str(), s.quality.c_str(),
        s.record.path.empty() ? L"(default)" : s.record.path.c_str());
    if (!e.HasTarget() || want != e.Target() || s.quality != e.Quality()) {
        LogDebug(L"apply target: type=%s path=%s camName=%s capture=%s scaleMode=%s "
                 L"crop=(%d,%d,%d,%d)%s quality=%s playOnce=%d loop=%d",
            want.type.c_str(), want.path.c_str(), want.camName.c_str(),
            want.capture.c_str(), want.scaleMode.c_str(), want.cropX, want.cropY,
            want.cropW, want.cropH, want.cropKeepAspect ? L" keepAspect" : L"",
            s.quality.c_str(), want.playOnce ? 1 : 0, want.loop ? 1 : 0);
        e.SetTarget(want, s.quality);
    }
}

void CheckBorrowedReturn(HostPipelineEngine& e, const std::wstring& settingsPath)
{
    bool borrowedNow = false;
    ULONGLONG borrowTick = 0;
    {
        ATL::CComCritSecLock<ATL::CComAutoCriticalSection> guard(g_hotkeyCs);
        borrowedNow = g_hotkeyBorrowed;
        borrowTick = g_hotkeyBorrowTickMs;
    }
    if (borrowedNow) {
        bool ended = e.Src() && e.Src()->Ended();
        ULONGLONG nowBorrow = GetTickCount64();
        bool timedOut = borrowTick != 0 && nowBorrow - borrowTick > kBorrowMaxMs;
        if (ended)
            AutoReturnBorrowedVideo(settingsPath, L"video ended");
        else if (e.PhaseRef() == PipelineEngine::Phase::Fallback)
            AutoReturnBorrowedVideo(settingsPath, L"fallback during borrowed video");
        else if (timedOut)
            AutoReturnBorrowedVideo(settingsPath, L"borrow timeout");
    }
}

void SetStatus(const std::wstring& text)
{
    ATL::CComCritSecLock<ATL::CComAutoCriticalSection> guard(g_statusCs);
    g_statusText = text;
}

std::wstring GetStatus()
{
    ATL::CComCritSecLock<ATL::CComAutoCriticalSection> guard(g_statusCs);
    std::wstring s = g_statusText;
    return s;
}
