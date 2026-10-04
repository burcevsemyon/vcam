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
#include "CriticalSectionGuard.h"

void OnSettingsChanged(const Settings&) { SetEvent(g_dirty); }

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
        vcam::CsGuard guard(&g_hotkeyCs);
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
            if (el < 0) el = 0;
            wchar_t t[32];
            swprintf_s(t, L"%02lld:%02lld", el / 60, el % 60);
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
        return 0;
    case WM_REAPPLY_HOTKEY:
        ApplyHotkeyRegistration();
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_SETTINGS: OpenSettingsUi(); break;
        case ID_PREVIEW: OpenPreview(); break;
        case ID_AUTOSTART: ToggleAutostart(); break;
        case ID_ABOUT: ShowAbout(); break;
        case ID_EXIT:
            Log(L"[host] exit requested (tray menu)");
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
