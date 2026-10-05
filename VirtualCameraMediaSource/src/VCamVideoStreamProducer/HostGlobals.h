#pragma once

#include <windows.h>
#include <atlbase.h>

#include <string>

#include "Settings.h"
#include "SettingsWatcher.h"

inline constexpr wchar_t kMutexName[] = L"VCamVideoStreamProducer.Instance";
inline constexpr wchar_t kStopEventName[] = L"VCamVideoStreamProducer.Stop";
inline constexpr wchar_t kTrayClass[] = L"VCamVideoStreamProducerWnd";
inline constexpr wchar_t kTrayTip[] = L"VCam Video Stream Producer";

inline constexpr UINT WM_TRAYICON = WM_APP + 1;
inline constexpr UINT WM_REAPPLY_HOTKEY = WM_APP + 2;
inline constexpr UINT kHotkeyId = 1;
inline constexpr UINT kRecHotkeyId = 2;
inline constexpr UINT ID_STATUS = 101;
inline constexpr UINT ID_SETTINGS = 102;
inline constexpr UINT ID_PREVIEW = 103;
inline constexpr UINT ID_AUTOSTART = 104;
inline constexpr UINT ID_EXIT = 105;
inline constexpr UINT ID_ABOUT = 106;

inline constexpr DWORD kFrameMs = 33;
inline constexpr DWORD kSwitchWindowMs = 5000;
inline constexpr DWORD kOpenRetryMs = 250;
inline constexpr DWORD kFallbackRetryMs = 1000;
inline constexpr DWORD kConsumerStaleMs = 3000;
inline constexpr ULONGLONG kBorrowMaxMs = 30ULL * 60 * 1000;
inline constexpr wchar_t kAutostartTask[] = L"VCamHost";

extern HINSTANCE g_inst;
extern HWND g_hwnd;
extern ATL::CHandle g_mutex;
extern ATL::CHandle g_stop;
extern ATL::CHandle g_dirty;
extern ATL::CHandle g_worker;
extern SettingsWatcher g_watcher;
extern NOTIFYICONDATAW g_nid;
extern ATL::CHandle g_logFile;

extern ATL::CComAutoCriticalSection g_statusCs;
extern std::wstring g_statusText;

extern ATL::CComAutoCriticalSection g_hotkeyCs;
extern HotkeySection g_hotkey;
extern std::wstring g_hotkeyReturnType;
extern bool g_hotkeyBorrowed;
extern ULONGLONG g_hotkeyBorrowTickMs;

extern ATL::CComAutoCriticalSection g_settingsCs;
struct SettingsFileGuard {
    SettingsFileGuard();
    ~SettingsFileGuard();
private:
    ATL::CComCritSecLock<ATL::CComAutoCriticalSection> guard_;
};

extern RecordHotkeySection g_recHotkey;
