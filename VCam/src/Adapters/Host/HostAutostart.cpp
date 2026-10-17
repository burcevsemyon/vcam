#include <windows.h>
#include <shellapi.h>
#include <atlbase.h>

#include <string>
#include <vector>

#include "HostGlobals.h"
#include "HostLogging.h"
#include "HostUtils.h"
#include "HostAutostart.h"

int RunSchtasks(const std::wstring& args, bool elevate)
{
    wchar_t sys[MAX_PATH] = {};
    if (GetSystemDirectoryW(sys, MAX_PATH) == 0) return -1;
    std::wstring exe = std::wstring(sys) + L"\\schtasks.exe";
    if (!elevate) {
        std::wstring cmd = L"\"" + exe + L"\" " + args;
        std::vector<wchar_t> buf(cmd.begin(), cmd.end()); buf.emplace_back(0);
        STARTUPINFOW si = {}; si.cb = sizeof(si);
        si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION pi = {};
        if (!CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE,
                            CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
            return -1;
        ATL::CHandle th(pi.hThread);
        ATL::CHandle proc(pi.hProcess);
        WaitForSingleObject(proc, 15000);
        DWORD code = (DWORD)-1;
        GetExitCodeProcess(proc, &code);
        return (int)code;
    }
    SHELLEXECUTEINFOW sei = {};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb = L"runas";
    sei.lpFile = exe.c_str();
    sei.lpParameters = args.c_str();
    sei.nShow = SW_HIDE;
    if (!ShellExecuteExW(&sei))
        return GetLastError() == ERROR_CANCELLED ? 1223 : -1;
    if (sei.hProcess) {
        ATL::CHandle proc(sei.hProcess);
        WaitForSingleObject(proc, 30000);
        DWORD code = (DWORD)-1;
        GetExitCodeProcess(proc, &code);
        return (int)code;
    }
    return 0;
}

std::wstring AutostartTaskArgs(bool enabled)
{
    if (!enabled)
        return std::wstring(L"/Delete /TN ") + kAutostartTask + L" /F";
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    return std::wstring(L"/Create /TN ") + kAutostartTask + L" /TR \"\\\"" +
           exe + L"\\\"\" /SC ONLOGON /RL HIGHEST /F";
}

bool IsAutostartTaskPresent()
{
    return RunSchtasks(std::wstring(L"/Query /TN ") + kAutostartTask, false) == 0;
}

bool ApplyAutostart(bool enabled)
{
    if (IsAutostartTaskPresent() == enabled) return true;
    int rc = RunSchtasks(AutostartTaskArgs(enabled), false);
    if (rc != 0) rc = RunSchtasks(AutostartTaskArgs(enabled), true);
    if (rc != 0) {
        Log(L"autostart task apply failed (enabled=%d, rc=%d)", (int)enabled, rc);
        return false;
    }
    return true;
}

void ApplyAutostartFromSettings()
{
    std::wstring path = DefaultSettingsPath();
    if (path.empty()) { Log(L"settings path is empty - autostart skipped"); return; }
    Settings s;
    bool loaded = s.Load(path);
    if (!loaded && PathExists(path)) {
        Log(L"settings unreadable - autostart left untouched");
        return;
    }
    bool present = IsAutostartTaskPresent();
    if (present == s.autostart) {
        Log(L"autostart %s (Task Scheduler\\%s)",
            s.autostart ? L"enabled" : L"disabled", kAutostartTask);
    } else {
        Log(L"autostart mismatch (settings=%d, task=%s) - fix via tray menu",
            (int)s.autostart, present ? L"present" : L"absent");
    }
}

void ToggleAutostart()
{
    std::wstring path = DefaultSettingsPath();
    if (path.empty()) {
        MessageBoxW(g_hwnd, L"Не найден путь к settings.json (APPDATA).", L"VCam",
                    MB_OK | MB_ICONWARNING);
        return;
    }
    Settings s;
    bool loaded = s.Load(path);
    if (!loaded && PathExists(path)) {
        Log(L"toggle autostart: settings unreadable");
        MessageBoxW(g_hwnd, L"Не удалось прочитать settings.json — автозагрузка не изменена.", L"VCam",
                    MB_OK | MB_ICONWARNING);
        return;
    }
    bool want = !s.autostart;
    int rc = RunSchtasks(AutostartTaskArgs(want), false);
    if (rc != 0) {
        rc = RunSchtasks(AutostartTaskArgs(want), true);
        if (rc == 1223) { Log(L"autostart toggle: UAC declined"); return; }
    }
    if (rc != 0) {
        Log(L"toggle autostart failed (rc=%d)", rc);
        MessageBoxW(g_hwnd, L"Не удалось изменить задачу автозапуска (Task Scheduler).", L"VCam",
                    MB_OK | MB_ICONWARNING);
        return;
    }
    s.autostart = want;
    {
        SettingsFileGuard fsg;
        Settings fresh;
        if (!fresh.Load(path)) fresh = s;
        fresh.autostart = want;
        if (!fresh.Save(path)) {
            Log(L"toggle autostart: settings save failed (task already changed)");
            return;
        }
    }
    Log(L"autostart -> %s", want ? L"on" : L"off");
}
