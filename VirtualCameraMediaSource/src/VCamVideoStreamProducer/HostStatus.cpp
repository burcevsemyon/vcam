#include <windows.h>
#include <atlbase.h>

#include <string>

#include "HostGlobals.h"
#include "HostLogging.h"
#include "HostRecording.h"
#include "HostHotkey.h"
#include "HostStatus.h"

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
        Log(L"[host] record: stale command removed (not resuming after restart)");
    }
    if (!cp.empty()) {
        std::wstring proc = cp + L".processing";
        if (GetFileAttributesW(proc.c_str()) != INVALID_FILE_ATTRIBUTES) {
            DeleteFileW(proc.c_str());
            Log(L"[host] record: stale command processing removed (not resuming after restart)");
        }
    }
    std::wstring curPath;
    long long curStarted = 0;
    if (TryReadRecordState(curPath, curStarted)) {
        ClearRecordState();
        Log(L"[host] record: stale state removed (was: %s)", curPath.c_str());
    }
}

void ApplySettingsDiff(HostPipelineEngine& e, HotkeySection& curHotkey,
                       RecordHotkeySection& curRecHotkey)
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
        Log(L"[host] hotkey: manual switch away from borrowed video - borrow dropped");
        borrowed = false;
    }
    if (borrowed && want.type == L"video") want.playOnce = true;
    if (!e.HasTarget() || want != e.Target() || s.quality != e.Quality())
        e.SetTarget(want, s.quality);
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
