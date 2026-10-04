#include <windows.h>
#include <tlhelp32.h>
#include <atlbase.h>

#include <string>
#include <vector>

#include "HostGlobals.h"
#include "HostLogging.h"
#include "HostUtils.h"
#include "HostCameraLifecycle.h"
#include "SharedMemoryContract.h"
#include "MappedViewOfFilePtr.h"

bool IsRegistrarRunning()
{
    HANDLE raw = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (raw == INVALID_HANDLE_VALUE) return false;
    ATL::CHandle snap(raw);
    bool found = false;
    PROCESSENTRY32W pe = {};
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, L"Registrar.exe") == 0) { found = true; break; }
        } while (Process32NextW(snap, &pe));
    }
    return found;
}

bool IsConsumerActive()
{
    const wchar_t* prefixes[] = { L"Global\\", L"Local\\" };
    const wchar_t* base = wcschr(vcam::VCamSectionName, L'\\');
    base = base ? base + 1 : vcam::VCamSectionName;
    for (const wchar_t* pre : prefixes) {
        wchar_t name[MAX_PATH] = {};
        swprintf_s(name, L"%s%s", pre, base);
        ATL::CHandle h(OpenFileMappingW(FILE_MAP_READ, FALSE, name));
        if (!h) continue;
        bool active = false;
        vcam::MappedViewOfFilePtr view(MapViewOfFile(h, FILE_MAP_READ, 0, 0,
                                                    sizeof(vcam::VCamSectionHeader)));
        if (view) {
            auto* hdr = view.GetAs<vcam::VCamSectionHeader>();
            if (hdr->magic == vcam::VCamMagic) {
                ULONGLONG tick = hdr->readerLastActiveTick;
                ULONGLONG now = GetTickCount64();
                active = (tick != 0) && (now >= tick) && (now - tick) <= kConsumerStaleMs;
            }
        }
        if (active) return true;
    }
    return false;
}

void StartCameraHolder()
{
    if (IsRegistrarRunning()) {
        Log(L"[host] camera holder already running");
        return;
    }
    std::wstring p = FindHelperExe(L"Registrar.exe", L"build\\x64\\Release\\Registrar.exe");
    if (p.empty()) {
        Log(L"[host] Registrar.exe not found - camera will NOT be registered");
        return;
    }
    std::wstring cmd = L"\"" + p + L"\" add VCam hold-watch";
    std::vector<wchar_t> buf(cmd.begin(), cmd.end()); buf.emplace_back(0);
    STARTUPINFOW si = {}; si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        Log(L"[host] camera holder start failed: %s (%lu)", p.c_str(), GetLastError());
        return;
    }
    ATL::CHandle th(pi.hThread);
    ATL::CHandle proc(pi.hProcess);
    Log(L"[host] camera holder started: %s", p.c_str());
}

void StopCameraHolder()
{
    if (!IsRegistrarRunning()) {
        Log(L"[host] camera holder not running - camera already gone");
        return;
    }
    if (IsConsumerActive()) {
        Log(L"[host] consumers active - camera kept (writers stopped -> NO SIGNAL in 7s)");
        return;
    }
    wchar_t sys[MAX_PATH] = {};
    GetSystemDirectoryW(sys, MAX_PATH);
    std::wstring cmd = std::wstring(L"\"") + sys + L"\\taskkill.exe\" /IM Registrar.exe /F";
    std::vector<wchar_t> buf(cmd.begin(), cmd.end()); buf.emplace_back(0);
    STARTUPINFOW si = {}; si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        Log(L"[host] taskkill(Registrar) failed: %lu", GetLastError());
        return;
    }
    ATL::CHandle th(pi.hThread);
    ATL::CHandle proc(pi.hProcess);
    WaitForSingleObject(proc, 10000);
    DWORD rc = 1;
    GetExitCodeProcess(proc, &rc);
    if (rc == 0 && !IsRegistrarRunning())
        Log(L"[host] camera holder stopped - camera removed from device list");
    else
        Log(L"[host] taskkill(Registrar) rc=%lu - holder may still be running (elevated?)", rc);
}
