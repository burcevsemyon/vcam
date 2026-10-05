#include <windows.h>
#include <shellapi.h>
#include <atlbase.h>

#include <cstdio>
#include <string>
#include <vector>

#include "FrameWriter.h"
#include "HostPipelineEngine.h"
#include "Mp4Recorder.h"
#include "ProducerApi.h"
#include "Settings.h"
#include "SettingsWatcher.h"
#include "SharedMemoryContract.h"
#include "MappedViewOfFilePtr.h"

#include "HostGlobals.h"
#include "HostLogging.h"
#include "HostRecording.h"
#include "HostHotkey.h"
#include "HostCameraLifecycle.h"
#include "HostAutostart.h"
#include "HostStatus.h"
#include "HostUtils.h"
#include "HostTray.h"

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "version.lib")

HINSTANCE g_inst = nullptr;
HWND g_hwnd = nullptr;
ATL::CHandle g_mutex;
ATL::CHandle g_stop;
ATL::CHandle g_dirty;
ATL::CHandle g_worker;
SettingsWatcher g_watcher;
NOTIFYICONDATAW g_nid = {};
ATL::CHandle g_logFile;

ATL::CComAutoCriticalSection g_statusCs;
std::wstring g_statusText = L"Нет сигнала (старт)";

ATL::CComAutoCriticalSection g_hotkeyCs;
HotkeySection g_hotkey;
std::wstring g_hotkeyReturnType = L"static";
bool g_hotkeyBorrowed = false;
ULONGLONG g_hotkeyBorrowTickMs = 0;

ATL::CComAutoCriticalSection g_settingsCs;
SettingsFileGuard::SettingsFileGuard() : guard_(g_settingsCs) {}
SettingsFileGuard::~SettingsFileGuard() = default;

RecordHotkeySection g_recHotkey;

DWORD WINAPI WorkerProc(LPVOID)
{
    HostPipelineEngine engine;
    engine.Open();

    CleanupStaleRecordFiles();

    HANDLE waits[2] = { static_cast<HANDLE>(g_stop), static_cast<HANDLE>(g_dirty) };
    bool first = true;
    DWORD timeout = 0;
    const std::wstring settingsPath = DefaultSettingsPath();
    HotkeySection curHotkey;
    RecordHotkeySection curRecHotkey;
    for (;;) {
        DWORD r = WaitForMultipleObjects(2, waits, FALSE, timeout);
        if (r == WAIT_OBJECT_0) break;
        if (first || r == WAIT_OBJECT_0 + 1) {
            first = false;
            ApplySettingsDiff(engine, curHotkey, curRecHotkey);
        }
        {
            std::wstring rcmd, rpath;
            if (ConsumeRecordCommand(rcmd, rpath)) {
                if (rcmd == L"start") engine.StartRecording(rpath);
                else if (rcmd == L"stop") engine.StopRecording(L"команда UI/хоткея");
                else Log(L"[host] record: unknown command ignored");
            }
        }
        timeout = engine.Step();
        CheckBorrowedReturn(engine, settingsPath);
    }

    if (engine.IsRecording()) engine.StopRecording(L"выход хоста");
    engine.Close();
    Log(L"[host] worker stopped");
    return 0;
}

void LogTokenState()
{
    HANDLE rawToken = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &rawToken)) {
        Log(L"[host] token: OpenProcessToken failed: %lu", GetLastError());
        return;
    }
    ATL::CHandle token(rawToken);
    int elevated = 0;
    TOKEN_ELEVATION elev = {};
    DWORD sz = 0;
    if (GetTokenInformation(token, TokenElevation, &elev, sizeof(elev), &sz))
        elevated = elev.TokenIsElevated ? 1 : 0;

    int priv = 0;
    LUID luid = {};
    if (LookupPrivilegeValueW(nullptr, SE_CREATE_GLOBAL_NAME, &luid)) {
        sz = 0;
        GetTokenInformation(token, TokenPrivileges, nullptr, 0, &sz);
        std::vector<BYTE> buf(sz);
        if (sz && GetTokenInformation(token, TokenPrivileges, buf.data(), sz, &sz)) {
            auto* tp = reinterpret_cast<TOKEN_PRIVILEGES*>(buf.data());
            for (DWORD i = 0; i < tp->PrivilegeCount; ++i) {
                if (memcmp(&tp->Privileges[i].Luid, &luid, sizeof(LUID)) == 0) {
                    priv = (tp->Privileges[i].Attributes & SE_PRIVILEGE_ENABLED) ? 2 : 1;
                    break;
                }
            }
        }
    }
    Log(L"[host] token: elevated=%d SeCreateGlobalPrivilege=%d", elevated, priv);
}

int APIENTRY wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int)
{
    g_inst = hInstance;
    InitLogging();
    Log(L"[host] VCamVideoStreamProducer starting");
    LogTokenState();

    SetLastError(ERROR_SUCCESS);
    g_mutex.Attach(CreateMutexW(nullptr, FALSE, kMutexName));
    if (static_cast<HANDLE>(g_mutex) == nullptr) {
        Log(L"[host] CreateMutex failed: %lu", GetLastError());
        return 1;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        Log(L"[host] already running - second instance exits");
        MessageBoxW(nullptr,
                    L"Хост VCam уже запущен (VCamVideoStreamProducer.exe).\n"
                    L"Используйте значок в трее рядом с часами.",
                    L"VCam Video Stream Producer", MB_OK | MB_ICONINFORMATION);
        g_mutex.Close();
        return 0;
    }
    WaitForSingleObject(static_cast<HANDLE>(g_mutex), INFINITE);

    g_stop.Attach(CreateEventW(nullptr, TRUE, FALSE, kStopEventName));
    if (static_cast<HANDLE>(g_stop) == nullptr) {
        Log(L"[host] CreateEvent(Stop) failed: %lu", GetLastError());
        ReleaseMutex(g_mutex);
        g_mutex.Close();
        return 1;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) ResetEvent(g_stop);
    g_dirty.Attach(CreateEventW(nullptr, FALSE, FALSE, nullptr));

    ApplyAutostartFromSettings();

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = g_inst;
    wc.hIcon = LoadIconW(g_inst, MAKEINTRESOURCEW(1));
    if (!wc.hIcon) wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.hIconSm = wc.hIcon;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kTrayClass;
    if (!RegisterClassExW(&wc)) {
        Log(L"[host] RegisterClassEx failed: %lu", GetLastError());
    }
    g_hwnd = CreateWindowExW(0, kTrayClass, L"VCamVideoStreamProducer", WS_OVERLAPPED,
                             0, 0, 0, 0, nullptr, nullptr, g_inst, nullptr);
    if (!g_hwnd) {
        Log(L"[host] CreateWindowEx failed: %lu", GetLastError());
        g_dirty.Close();
        g_stop.Close();
        ReleaseMutex(g_mutex);
        g_mutex.Close();
        return 1;
    }

    {
        Settings initS;
        if (initS.Load(DefaultSettingsPath())) {
            ATL::CComCritSecLock<ATL::CComAutoCriticalSection> guard(g_hotkeyCs);
            g_hotkey = initS.hotkey;
            g_recHotkey = initS.recordHotkey;
        }
    }
    ApplyHotkeyRegistration();

    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = g_hwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;
    g_nid.hIcon = LoadIconW(g_inst, MAKEINTRESOURCEW(1));
    if (!g_nid.hIcon) g_nid.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wcsncpy_s(g_nid.szTip, kTrayTip, _TRUNCATE);
    if (!Shell_NotifyIconW(NIM_ADD, &g_nid)) Log(L"[host] Shell_NotifyIcon(add) failed");
    Log(L"[host] tray icon added");

    StartCameraHolder();

    std::wstring settingsPath = DefaultSettingsPath();
    if (!g_watcher.Start(settingsPath, OnSettingsChanged)) {
        Log(L"[host] settings watcher not started (settings path is empty?)");
    } else {
        Log(L"[host] watching: %s", settingsPath.c_str());
    }

    g_worker.Attach(CreateThread(nullptr, 0, WorkerProc, nullptr, 0, nullptr));
    if (static_cast<HANDLE>(g_worker) == nullptr)
        Log(L"[host] CreateThread(worker) failed: %lu", GetLastError());

    for (;;) {
        MSG msg;
        bool quit = false;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { quit = true; break; }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (quit) break;
        HANDLE hStop = static_cast<HANDLE>(g_stop);
        DWORD r = MsgWaitForMultipleObjects(1, &hStop, FALSE, INFINITE, QS_ALLINPUT);
        if (r == WAIT_OBJECT_0) break;
    }

    Log(L"[host] shutting down");
    SetEvent(g_stop);
    g_watcher.Stop();
    if (static_cast<HANDLE>(g_worker) != nullptr) {
        if (WaitForSingleObject(static_cast<HANDLE>(g_worker), 8000) != WAIT_OBJECT_0) {
            Log(L"[host] worker did not stop in 8000 ms; waiting indefinitely");
            WaitForSingleObject(static_cast<HANDLE>(g_worker), INFINITE);
        }
        g_worker.Close();
    }
    Shell_NotifyIconW(NIM_DELETE, &g_nid);
    Log(L"[host] tray icon removed");
    UnregisterHotKey(g_hwnd, kHotkeyId);
    UnregisterHotKey(g_hwnd, kRecHotkeyId);
    DestroyWindow(g_hwnd);
    g_hwnd = nullptr;

    g_dirty.Close();
    g_stop.Close();
    ReleaseMutex(g_mutex);
    g_mutex.Close();
    Log(L"[host] exit");
    g_logFile.Close();
    return 0;
}
