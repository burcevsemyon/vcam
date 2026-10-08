#include <windows.h>
#include <atlbase.h>

#include <string>

#include "HostGlobals.h"
#include "HostLogging.h"
#include "HostRecording.h"
#include "HostHotkey.h"
#include "FailOpenCounters.h"

std::wstring HotkeyStatePath()
{
    wchar_t appdata[MAX_PATH] = {};
    DWORD n = GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return {};
    return std::wstring(appdata) + L"\\VCam\\hotkey_state.json";
}

template <typename T>
std::wstring HotkeyDisplay(const T& hk)
{
    std::wstring s;
    if (hk.modifiers & MOD_CONTROL) s += L"Ctrl+";
    if (hk.modifiers & MOD_ALT) s += L"Alt+";
    if (hk.modifiers & MOD_SHIFT) s += L"Shift+";
    if (hk.modifiers & MOD_WIN) s += L"Win+";
    wchar_t key[32] = {};
    if ((hk.vk >= '0' && hk.vk <= '9') || (hk.vk >= 'A' && hk.vk <= 'Z')) {
        swprintf_s(key, L"%c", (wchar_t)hk.vk);
    } else if (hk.vk >= VK_F1 && hk.vk <= VK_F24) {
        swprintf_s(key, L"F%d", hk.vk - VK_F1 + 1);
    } else {
        switch (hk.vk) {
        case VK_SPACE: wcscpy_s(key, L"Space"); break;
        case VK_RETURN: wcscpy_s(key, L"Enter"); break;
        case VK_TAB: wcscpy_s(key, L"Tab"); break;
        case VK_ESCAPE: wcscpy_s(key, L"Esc"); break;
        case VK_LEFT: wcscpy_s(key, L"Left"); break;
        case VK_RIGHT: wcscpy_s(key, L"Right"); break;
        case VK_UP: wcscpy_s(key, L"Up"); break;
        case VK_DOWN: wcscpy_s(key, L"Down"); break;
        default: swprintf_s(key, L"VK 0x%02X", (unsigned)hk.vk); break;
        }
    }
    s += key;
    return s;
}

void WriteHotkeyState(const std::wstring& returnTo)
{
    std::wstring path = HotkeyStatePath();
    if (path.empty()) return;
    size_t slash = path.find_last_of(L"\\/");
    if (slash != std::wstring::npos) CreateDirectoryW(path.substr(0, slash).c_str(), nullptr);
    std::string utf8 = "{\"borrowed\":true,\"returnTo\":\"";
    for (wchar_t c : returnTo) utf8 += (c < 0x80) ? (char)c : '?';
    utf8 += "\"}\n";
    HANDLE raw = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                             CREATE_ALWAYS, 0, nullptr);
    if (raw == INVALID_HANDLE_VALUE) {
        Log(L"hotkey state write failed: %lu", GetLastError());
        return;
    }
    ATL::CHandle h(raw);
    DWORD written = 0;
    WriteFile(h, utf8.data(), (DWORD)utf8.size(), &written, nullptr);
}

void ClearHotkeyState()
{
    std::wstring path = HotkeyStatePath();
    if (!path.empty()) DeleteFileW(path.c_str());
}

void ApplyHotkeyRegistration()
{
    if (!g_hwnd) return;
    UnregisterHotKey(g_hwnd, kHotkeyId);
    UnregisterHotKey(g_hwnd, kRecHotkeyId);
    UnregisterHotKey(g_hwnd, kVideoHotkeyId);
    UnregisterHotKey(g_hwnd, kSourceStaticHotkeyId);
    UnregisterHotKey(g_hwnd, kSourceVideoHotkeyId);
    UnregisterHotKey(g_hwnd, kSourceCameraHotkeyId);
    HotkeySection hk;
    RecordHotkeySection rk;
    VideoHotkeySection vk;
    SourceSwitchHotkeySection shkStatic, shkVideo, shkCamera;
    {
        ATL::CComCritSecLock<ATL::CComAutoCriticalSection> guard(g_hotkeyCs);
        hk = g_hotkey;
        rk = g_recHotkey;
        vk = g_videoHotkey;
        shkStatic = g_sourceStaticHotkey;
        shkVideo = g_sourceVideoHotkey;
        shkCamera = g_sourceCameraHotkey;
    }
    if (RegisterHotKey(g_hwnd, kHotkeyId, (UINT)hk.modifiers, (UINT)hk.vk)) {
        Log(L"hotkey registered: %s", HotkeyDisplay(hk).c_str());
    } else {
        vcam::IncFailOpen(vcam::FailOpen::HotkeyBusy);
        Log(L"hotkey RegisterHotKey(%s) failed: %lu - hotkey disabled until settings change",
            HotkeyDisplay(hk).c_str(), GetLastError());
    }
    if (RegisterHotKey(g_hwnd, kRecHotkeyId, (UINT)rk.modifiers, (UINT)rk.vk)) {
        Log(L"record hotkey registered: %s", HotkeyDisplay(rk).c_str());
    } else {
        vcam::IncFailOpen(vcam::FailOpen::HotkeyBusy);
        Log(L"record hotkey RegisterHotKey(%s) failed: %lu - record hotkey disabled until settings change",
            HotkeyDisplay(rk).c_str(), GetLastError());
    }
    if (RegisterHotKey(g_hwnd, kVideoHotkeyId, (UINT)vk.modifiers, (UINT)vk.vk)) {
        Log(L"video play/pause hotkey registered: %s", HotkeyDisplay(vk).c_str());
    } else {
        vcam::IncFailOpen(vcam::FailOpen::HotkeyBusy);
        Log(L"video hotkey RegisterHotKey(%s) failed: %lu - video hotkey disabled until settings change",
            HotkeyDisplay(vk).c_str(), GetLastError());
    }
    // Хоткеи переключения источника: те же правила, что у остальных.
    struct { UINT id; const SourceSwitchHotkeySection* hk; const wchar_t* what; } shks[] = {
        { kSourceStaticHotkeyId, &shkStatic, L"source static" },
        { kSourceVideoHotkeyId, &shkVideo, L"source video" },
        { kSourceCameraHotkeyId, &shkCamera, L"source camera" },
    };
    for (auto& shk : shks) {
        if (RegisterHotKey(g_hwnd, shk.id, (UINT)shk.hk->modifiers, (UINT)shk.hk->vk)) {
            Log(L"%s hotkey registered: %s", shk.what, HotkeyDisplay(*shk.hk).c_str());
        } else {
            vcam::IncFailOpen(vcam::FailOpen::HotkeyBusy);
            Log(L"%s hotkey RegisterHotKey(%s) failed: %lu - hotkey disabled until settings change",
                shk.what, HotkeyDisplay(*shk.hk).c_str(), GetLastError());
        }
    }
}

bool AutoReturnBorrowedVideo(const std::wstring& settingsPath, const wchar_t* why)
{
    bool borrowed = false;
    std::wstring ret;
    {
        ATL::CComCritSecLock<ATL::CComAutoCriticalSection> guard(g_hotkeyCs);
        borrowed = g_hotkeyBorrowed;
        ret = g_hotkeyReturnType;
    }
    if (!borrowed) return true;
    if (ret.empty()) ret = L"static";
    Settings back;
    bool loaded = !settingsPath.empty() && back.Load(settingsPath);
    if (!loaded) {
        Log(L"hotkey: %s, settings unreadable - retry later", why);
        return false;
    }
    if (back.sourceType != L"video") {
        {
            ATL::CComCritSecLock<ATL::CComAutoCriticalSection> guard(g_hotkeyCs);
            g_hotkeyBorrowed = false;
            g_hotkeyBorrowTickMs = 0;
        }
        ClearHotkeyState();
        return true;
    }
    back.sourceType = ret;
    bool saved;
    {
        SettingsFileGuard fsg;
        saved = back.Save(settingsPath);
    }
    if (!saved) {
        Log(L"hotkey: %s, auto-return save failed - retry later", why);
        return false;
    }
    {
        ATL::CComCritSecLock<ATL::CComAutoCriticalSection> guard(g_hotkeyCs);
        g_hotkeyBorrowed = false;
        g_hotkeyBorrowTickMs = 0;
    }
    ClearHotkeyState();
    Log(L"hotkey: %s, auto-return to %s", why, ret.c_str());
    return true;
}

void OnHotkeyPressed()
{
    std::wstring path = DefaultSettingsPath();
    if (path.empty()) {
        Log(L"hotkey: settings path is empty");
        return;
    }
    SettingsFileGuard fsg;
    Settings s;
    if (!s.Load(path)) {
        Log(L"hotkey: settings unreadable - ignored");
        return;
    }
    std::wstring display;
    bool borrowed = false;
    std::wstring retType;
    {
        ATL::CComCritSecLock<ATL::CComAutoCriticalSection> guard(g_hotkeyCs);
        display = HotkeyDisplay(g_hotkey);
        borrowed = g_hotkeyBorrowed;
        retType = g_hotkeyReturnType;
    }

    if (!borrowed) {
        if (s.sourceType != L"video") {
            std::wstring from = s.sourceType;
            {
                ATL::CComCritSecLock<ATL::CComAutoCriticalSection> guard(g_hotkeyCs);
                g_hotkeyReturnType = from;
                g_hotkeyBorrowed = true;
                g_hotkeyBorrowTickMs = GetTickCount64();
            }
            s.sourceType = L"video";
            if (!s.Save(path)) {
                {
                    ATL::CComCritSecLock<ATL::CComAutoCriticalSection> guard(g_hotkeyCs);
                    g_hotkeyBorrowed = false;
                    g_hotkeyBorrowTickMs = 0;
                }
                Log(L"hotkey %s: settings save failed - borrow cancelled",
                    display.c_str());
                return;
            }
            WriteHotkeyState(from);
            Log(L"hotkey %s: %s -> video (play-once, auto-return to %s)",
                display.c_str(), from.c_str(), from.c_str());
        } else {
            s.sourceType = L"static";
            if (s.Save(path))
                Log(L"hotkey %s: video -> static (manual video, no borrow)",
                    display.c_str());
            else
                Log(L"hotkey %s: settings save failed", display.c_str());
        }
        return;
    }

    std::wstring back = retType.empty() ? L"static" : retType;
    s.sourceType = back;
    if (!s.Save(path)) {
        Log(L"hotkey %s: early return save failed - borrow kept", display.c_str());
        return;
    }
    {
        ATL::CComCritSecLock<ATL::CComAutoCriticalSection> guard(g_hotkeyCs);
        g_hotkeyBorrowed = false;
        g_hotkeyBorrowTickMs = 0;
    }
    ClearHotkeyState();
    Log(L"hotkey %s: early return video -> %s", display.c_str(), back.c_str());
}

void OnRecordHotkeyPressed()
{
    std::wstring display;
    {
        ATL::CComCritSecLock<ATL::CComAutoCriticalSection> guard(g_hotkeyCs);
        display = HotkeyDisplay(g_recHotkey);
    }

    std::wstring curPath;
    long long curStarted = 0;
    if (TryReadRecordState(curPath, curStarted)) {
        if (!WriteRecordCommand(L"stop", L""))
            Log(L"record hotkey %s: stop command write failed", display.c_str());
        else
            Log(L"record hotkey %s: stop requested", display.c_str());
        return;
    }
    std::wstring want;
    std::wstring sp = DefaultSettingsPath();
    Settings s;
    if (!sp.empty() && s.Load(sp)) want = s.record.path;
    if (!WriteRecordCommand(L"start", want))
        Log(L"record hotkey %s: start command write failed", display.c_str());
    else
        Log(L"record hotkey %s: start requested", display.c_str());
}

// Play/pause видео: только взводим флаг — toggle применяет worker к живому
// источнику (у UI-потока нет мьютекса источника; seek только в DecodeLoop).
void OnVideoPlayHotkeyPressed()
{
    std::wstring display;
    {
        ATL::CComCritSecLock<ATL::CComAutoCriticalSection> guard(g_hotkeyCs);
        display = HotkeyDisplay(g_videoHotkey);
        g_videoToggleRequested = true;
    }
    // Разбудить worker немедленно (иначе ждёт свой кадровый таймаут).
    if (g_dirty) SetEvent(g_dirty);
    Log(L"video hotkey %s: play/pause toggle requested", display.c_str());
}
