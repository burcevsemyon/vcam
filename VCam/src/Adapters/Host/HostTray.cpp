#include <windows.h>
#include <atlbase.h>

#include <ctime>
#include <string>

#include "HostGlobals.h"
#include "HostLogging.h"
#include "HostRecording.h"
#include "HostHotkey.h"
#include "HostAutostart.h"
#include "HostUtils.h"
#include "HostCameraLifecycle.h"
#include "HostStatus.h"
#include "HostTray.h"
#include "TraySourceMenu.h"

void OnSettingsChanged(const Settings&) { SetEvent(g_dirty); }

static std::wstring FormatElapsedTime(long long seconds)
{
    if (seconds < 0) seconds = 0;
    wchar_t buf[32];
    swprintf_s(buf, L"%02lld:%02lld", seconds / 60, seconds % 60);
    return buf;
}

void ShowTrayMenu(HWND hwnd)
{
    POINT pt = {};
    GetCursorPos(&pt);

    Settings s;
    s.Load(DefaultSettingsPath());

    std::wstring status = GetStatus();
    bool borrowedMenu = false;
    std::wstring retMenu;
    {
        ATL::CComCritSecLock<ATL::CComAutoCriticalSection> guard(g_hotkeyCs);
        borrowedMenu = g_hotkeyBorrowed;
        retMenu = g_hotkeyReturnType;
    }
    if (borrowedMenu)
        status += L" [видео по хоткею, автовозврат → " +
                  (retMenu.empty() ? std::wstring(L"static") : retMenu) + L"]";
    {
        std::wstring recPath;
        long long recStarted = 0;
        if (TryReadRecordState(recPath, recStarted)) {
            long long el = (long long)time(nullptr) - recStarted;
            std::wstring t = FormatElapsedTime(el);
            size_t bs = recPath.find_last_of(L"\\/");
            std::wstring fn = (bs == std::wstring::npos) ? recPath : recPath.substr(bs + 1);
            status += L" [● REC ";
            status += t;
            status += L" ";
            status += fn;
            status += L"]";
        }
    }
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING | MF_GRAYED | MF_DISABLED, ID_STATUS, status.c_str());
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, ID_SETTINGS, L"Настройки VCam…");
    AppendMenuW(menu, MF_STRING, ID_PREVIEW, L"Окно предпросмотра…");
    AppendSourceSubmenu(menu, s, ID_SOURCE_STATIC, ID_SOURCE_VIDEO, ID_SOURCE_CAMERA);
    AppendMenuW(menu, MF_STRING | (s.autostart ? MF_CHECKED : 0), ID_AUTOSTART, L"Автозагрузка");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, ID_ABOUT, L"О программе…");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, ID_EXIT, L"Выход");

    SetForegroundWindow(hwnd);
    UINT cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);
    DestroyMenu(menu);
    PostMessageW(hwnd, WM_NULL, 0, 0);
    if (cmd) SendMessageW(hwnd, WM_COMMAND, cmd, 0);
}

// origin — источник команды для лога: "tray" (подменю) или "hotkey".
void SwitchSource(const wchar_t* type, const wchar_t* origin)
{
    std::wstring path = DefaultSettingsPath();
    if (path.empty()) {
        Log(L"%s: settings path is empty", origin);
        return;
    }
    SettingsFileGuard fsg;
    SourceSwitchResult r = ApplySourceSwitch(path, type);
    if (r == SourceSwitchResult::LoadFailed) {
        Log(L"%s: settings unreadable - source switch ignored", origin);
        return;
    }
    if (r == SourceSwitchResult::SaveFailed) {
        Log(L"%s: source switch -> %s: settings save failed", origin, type);
        return;
    }
    // явный выбор сбрасывает borrow, иначе автовозврат перезапишет выбор
    bool borrowed;
    {
        ATL::CComCritSecLock<ATL::CComAutoCriticalSection> guard(g_hotkeyCs);
        borrowed = g_hotkeyBorrowed;
        g_hotkeyBorrowed = false;
        g_hotkeyBorrowTickMs = 0;
    }
    if (borrowed) {
        ClearHotkeyState();
        Log(L"%s: source switch -> %s (borrow dropped)", origin, type);
    } else if (r == SourceSwitchResult::Saved) {
        Log(L"%s: source switch -> %s", origin, type);
    }
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_TRAYICON:
        if (lp == WM_RBUTTONUP) ShowTrayMenu(hwnd);
        else if (lp == WM_LBUTTONDBLCLK) OpenSettingsUi();
        return 0;
    case WM_HOTKEY:
        if (wp == kHotkeyId) OnHotkeyPressed();
        else if (wp == kRecHotkeyId) OnRecordHotkeyPressed();
        else if (wp == kVideoHotkeyId) OnVideoPlayHotkeyPressed();
        else if (wp == kSourceStaticHotkeyId) SwitchSource(L"static", L"hotkey");
        else if (wp == kSourceVideoHotkeyId) SwitchSource(L"video", L"hotkey");
        else if (wp == kSourceCameraHotkeyId) SwitchSource(L"camera", L"hotkey");
        return 0;
    case WM_REAPPLY_HOTKEY:
        ApplyHotkeyRegistration();
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_SETTINGS: OpenSettingsUi(); break;
        case ID_PREVIEW: OpenPreview(); break;
        case ID_SOURCE_STATIC: SwitchSource(L"static", L"tray"); break;
        case ID_SOURCE_VIDEO: SwitchSource(L"video", L"tray"); break;
        case ID_SOURCE_CAMERA: SwitchSource(L"camera", L"tray"); break;
        case ID_AUTOSTART: ToggleAutostart(); break;
        case ID_ABOUT: ShowAbout(); break;
        case ID_EXIT:
            Log(L"exit requested (tray menu)");
            SetEvent(g_stop);
            StopCameraHolder();
            break;
        }
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
