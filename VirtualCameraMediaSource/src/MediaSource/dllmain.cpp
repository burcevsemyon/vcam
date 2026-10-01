#include <windows.h>
#include <atlbase.h>
#include <atlcom.h>
#include <objbase.h>
#include <cstdio>
#include <cstdarg>
#include "MediaSource.h"
#include "ModuleLifetime.h"
#include "Activator.h"
#include "GUIDs.h"

static volatile LONG g_moduleLockCount = 0;
static volatile LONG g_objectCount = 0;
static HMODULE s_hModule = nullptr;

void VCamObjectInc() { InterlockedIncrement(&g_objectCount); }
void VCamObjectDec() { InterlockedDecrement(&g_objectCount); }

// Debug-only verbose diagnostics (TEMP DIAGNOSTIC): every COM call is logged
// to msrc_diag.log. Release builds compile VCamDiagLog as a no-op; the three
// e2e-critical events go through VCamDiagEvent (always logged).
static void DiagEmit(const wchar_t* fmt, va_list args)
{
    wchar_t line[512];
    int len = _vsnwprintf(line, 511, fmt, args);
    if (len < 0) len = 511;
    line[len] = L'\0';
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t full[768];
    swprintf_s(full, 768, L"[%02d:%02d:%02d.%03d] pid=%u tid=%u %ls\n",
        st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, GetCurrentProcessId(), GetCurrentThreadId(), line);
    const wchar_t* paths[] = {
        L"C:\\Users\\Semen\\AppData\\Local\\Temp\\opencode\\msrc_diag.log",
        L"C:\\Windows\\Temp\\vcam_ls_load.log"
    };
    for (auto p : paths) {
        FILE* f = nullptr;
        if (_wfopen_s(&f, p, L"a") == 0 && f != nullptr) {
            fputws(full, f);
            fclose(f);
        }
    }
}

void VCamDiagLog(const wchar_t* fmt, ...)
{
#ifndef NDEBUG
    va_list args;
    va_start(args, fmt);
    DiagEmit(fmt, args);
    va_end(args);
#else
    (void)fmt;
#endif
}

void VCamDiagEvent(const wchar_t* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    DiagEmit(fmt, args);
    va_end(args);
}

void VCamDiagGuid(const GUID* g, wchar_t* out, size_t cch)
{
    swprintf_s(out, cch, L"{%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
        g->Data1, g->Data2, g->Data3,
        g->Data4[0], g->Data4[1], g->Data4[2], g->Data4[3],
        g->Data4[4], g->Data4[5], g->Data4[6], g->Data4[7]);
}

class CVCamClassFactory : public IClassFactory
{
    LONG m_ref = 1;

public:
    CVCamClassFactory() { VCamObjectInc(); }

private:

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override
    {
        if (ppv == nullptr) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IClassFactory) {
            *ppv = static_cast<IClassFactory*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&m_ref); }
    STDMETHODIMP_(ULONG) Release() override
    {
        LONG r = InterlockedDecrement(&m_ref);
        if (r == 0) {
            VCamObjectDec();
            delete this;
        }
        return r;
    }
    STDMETHODIMP CreateInstance(IUnknown* pUnkOuter, REFIID riid, void** ppv) override
    {
        if (ppv == nullptr) return E_POINTER;
        *ppv = nullptr;
        if (pUnkOuter != nullptr) return CLASS_E_NOAGGREGATION;

        wchar_t riidStr[64];
        VCamDiagGuid(&riid, riidStr, 64);

        const bool wantActivator =
            (riid == IID_IMFActivate) || (riid == IID_IMFAttributes);

        if (wantActivator)
        {
            ATL::CComPtr<CVCamActivator> pAct;
            pAct.Attach(new (std::nothrow) CVCamActivator());
            if (pAct == nullptr) return E_OUTOFMEMORY;
            pAct.p->AddRef(); // one local ref; CComPtr releases it below (was pAct->Release())
            HRESULT hr = pAct->FinalConstruct();
            if (SUCCEEDED(hr)) {
                hr = pAct->QueryInterface(riid, ppv);
            }
            pAct = nullptr;
            VCamDiagLog(L"CreateInstance(activator) riid=%s -> 0x%08X", riidStr, hr);
            return hr;
        }

        ATL::CComPtr<CMediaSource> pSource;
        pSource.Attach(new (std::nothrow) CMediaSource());
        if (pSource == nullptr) return E_OUTOFMEMORY;
        pSource.p->AddRef(); // one local ref; CComPtr releases it below (was pSource->Release())
        HRESULT hr = pSource->FinalConstruct();
        if (SUCCEEDED(hr)) {
            hr = pSource->QueryInterface(riid, ppv);
        }
        pSource = nullptr;
        VCamDiagLog(L"CreateInstance(source) riid=%s -> 0x%08X", riidStr, hr);
        return hr;
    }
    STDMETHODIMP LockServer(BOOL fLock) override
    {
        if (fLock) InterlockedIncrement(&g_moduleLockCount);
        else InterlockedDecrement(&g_moduleLockCount);
        return S_OK;
    }
};

STDAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, void** ppv)
{
    wchar_t clsidStr[64], riidStr[64];
    VCamDiagGuid(&rclsid, clsidStr, 64);
    VCamDiagGuid(&riid, riidStr, 64);
    VCamDiagLog(L"DllGetClassObject rclsid=%s riid=%s", clsidStr, riidStr);

    if (ppv == nullptr) return E_POINTER;
    *ppv = nullptr;

    if (rclsid != CLSID_VCamMediaSource) return CLASS_E_CLASSNOTAVAILABLE;
    if (riid != IID_IUnknown && riid != IID_IClassFactory) return E_NOINTERFACE;

    CVCamClassFactory* pFactory = new (std::nothrow) CVCamClassFactory;
    if (pFactory == nullptr) return E_OUTOFMEMORY;
    *ppv = pFactory;
    return S_OK;
}

STDAPI DllCanUnloadNow()
{
    // Unload is only safe when no COM objects (source/stream/activator/proxy/
    // class factory) are alive — not just when LockServer was balanced.
    return (g_moduleLockCount > 0 || g_objectCount > 0) ? S_FALSE : S_OK;
}

STDAPI DllRegisterServer()
{
    HKEY hKey = nullptr;
    LSTATUS lstat = RegCreateKeyExW(
        HKEY_CLASSES_ROOT,
        L"CLSID\\{B2B674D4-9CF0-461C-BDCE-3D56FBB41356}\\InprocServer32",
        0, nullptr, REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, &hKey, nullptr);
    if (lstat != ERROR_SUCCESS) return HRESULT_FROM_WIN32(lstat);

    wchar_t dllPath[MAX_PATH];
    GetModuleFileNameW(s_hModule, dllPath, MAX_PATH);
    lstat = RegSetValueExW(hKey, nullptr, 0, REG_SZ,
        (const BYTE*)dllPath, (DWORD)((wcslen(dllPath) + 1) * sizeof(wchar_t)));
    // ThreadingModel=Both: an empty/missing ThreadingModel makes COM marshal
    // every call from an MTA client through a proxy that answers E_NOINTERFACE
    // for IMFMediaSource2 (QI fails, SetMediaType never runs, no frames).
    if (lstat == ERROR_SUCCESS) {
        static const wchar_t kBoth[] = L"Both";
        lstat = RegSetValueExW(hKey, L"ThreadingModel", 0, REG_SZ,
            (const BYTE*)kBoth, (DWORD)((wcslen(kBoth) + 1) * sizeof(wchar_t)));
    }
    RegCloseKey(hKey);
    return (lstat == ERROR_SUCCESS) ? S_OK : HRESULT_FROM_WIN32(lstat);
}

STDAPI DllUnregisterServer()
{
    LSTATUS lstat = RegDeleteTreeW(
        HKEY_CLASSES_ROOT,
        L"CLSID\\{B2B674D4-9CF0-461C-BDCE-3D56FBB41356}");
    return (lstat == ERROR_SUCCESS) ? S_OK : HRESULT_FROM_WIN32(lstat);
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved)
{
    switch (ul_reason_for_call) {
    case DLL_PROCESS_ATTACH:
        s_hModule = hModule;
        VCamDiagLog(L"DllMain PROCESS_ATTACH (module=%p)", hModule);
        break;
    case DLL_PROCESS_DETACH:
        VCamDiagLog(L"DllMain PROCESS_DETACH (lpReserved=%p)", lpReserved);
        s_hModule = nullptr;
        break;
    }
    return TRUE;
}
