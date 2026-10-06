#include <windows.h>
#include <shellapi.h>
#include <atlbase.h>

#include <string>
#include <vector>

#include "HostGlobals.h"
#include "HostLogging.h"
#include "HostUtils.h"

bool PathExists(const std::wstring& p)
{
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

std::wstring ExeDirectory()
{
    wchar_t buf[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring p(buf);
    size_t slash = p.find_last_of(L"\\/");
    return slash == std::wstring::npos ? p : p.substr(0, slash);
}

std::wstring FindHelperExe(const wchar_t* fileName, const wchar_t* rel)
{
    std::wstring root = ExeDirectory();
    for (int i = 0; i < 10; i++) {
        if (i > 0) {
            size_t slash = root.find_last_of(L"\\/");
            if (slash == std::wstring::npos || slash < 3) break;
            root = root.substr(0, slash);
        }
        std::wstring direct = root + L"\\" + fileName;
        if (PathExists(direct)) return direct;
        if (rel) {
            std::wstring candidate = root + L"\\" + rel;
            if (PathExists(candidate)) return candidate;
        }
    }
    return std::wstring();
}

void LaunchHelper(const std::wstring& path)
{
    std::wstring dir = path.substr(0, path.find_last_of(L"\\/"));
    SHELLEXECUTEINFOW sei = {};
    sei.cbSize = sizeof(sei);
    sei.lpVerb = L"open";
    sei.lpFile = path.c_str();
    sei.lpDirectory = dir.c_str();
    sei.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&sei)) {
        Log(L"launch failed: %s (%lu)", path.c_str(), GetLastError());
        return;
    }
    Log(L"launched: %s", path.c_str());
}

void OpenSettingsUi()
{
    std::wstring p = FindHelperExe(
        L"VCamSettingsUi.exe",
        L"src\\VCamSettingsUi\\bin\\x64\\Release\\net10.0-windows\\VCamSettingsUi.exe");
    if (p.empty()) {
        Log(L"VCamSettingsUi.exe not found");
        MessageBoxW(g_hwnd, L"VCamSettingsUi.exe не найден рядом с хостом и в исходниках репозитория.",
                    L"VCam", MB_OK | MB_ICONWARNING);
        return;
    }
    LaunchHelper(p);
}

void OpenPreview()
{
    std::wstring p = FindHelperExe(L"VCamPreview.exe", L"build\\x64\\Release\\VCamPreview.exe");
    if (p.empty()) {
        Log(L"VCamPreview.exe not found");
        MessageBoxW(g_hwnd, L"VCamPreview.exe не найден рядом с хостом и в папке build\\x64\\Release.",
                    L"VCam", MB_OK | MB_ICONWARNING);
        return;
    }
    LaunchHelper(p);
}

std::wstring ProductVersionString()
{
    wchar_t mod[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, mod, MAX_PATH)) {
        DWORD dummy = 0;
        DWORD sz = GetFileVersionInfoSizeW(mod, &dummy);
        if (sz) {
            std::vector<BYTE> buf(sz);
            if (GetFileVersionInfoW(mod, 0, sz, buf.data())) {
                VS_FIXEDFILEINFO* ffi = nullptr;
                UINT len = 0;
                if (VerQueryValueW(buf.data(), L"\\", reinterpret_cast<void**>(&ffi), &len) && ffi) {
                    wchar_t out[64];
                    swprintf_s(out, L"%u.%u.%u", HIWORD(ffi->dwProductVersionMS),
                               LOWORD(ffi->dwProductVersionMS), HIWORD(ffi->dwProductVersionLS));
                    return out;
                }
            }
        }
    }
    return L"0.0.0";
}

void ShowAbout()
{
    std::wstring msg =
        L"VCam Virtual Camera — виртуальная камера Windows.\n"
        L"Версия " + ProductVersionString() + L"\n\n"
        L"Хост пишет кадры источника в общую память, MediaSource.dll\n"
        L"отдаёт их потребителям (Zoom, ktalk, Windows Камеры…).\n\n"
        L"Настройки и предпросмотр — двойной клик / ПКМ по иконке в трее.";
    MessageBoxW(g_hwnd, msg.c_str(), L"О программе — VCam", MB_OK | MB_ICONINFORMATION);
}
