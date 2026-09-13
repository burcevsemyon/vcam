#include <windows.h>
#include <combaseapi.h>
#include <mfapi.h>
#include <mfvirtualcamera.h>
#include <cstdio>
#include <cwchar>
#include <new>
#include "../Common/GUIDs.h"

static const GUID KSCATEGORY_VIDEO_CAMERA =
    { 0x06990ad0, 0xc7a0, 0x11d0, { 0x8a, 0x49, 0x00, 0xA0, 0xC9, 0x22, 0x31, 0x96 } };

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mf.lib")
#pragma comment(lib, "mfsensorgroup.lib")
#pragma comment(lib, "ole32.lib")

static int GuidToStringW(REFGUID rguid, wchar_t* pstr, int cchMax)
{
    return swprintf_s(pstr, cchMax, L"{%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
        rguid.Data1, rguid.Data2, rguid.Data3,
        rguid.Data4[0], rguid.Data4[1], rguid.Data4[2], rguid.Data4[3],
        rguid.Data4[4], rguid.Data4[5], rguid.Data4[6], rguid.Data4[7]);
}

extern const GUID IID_IMFAsyncCallback =
    { 0xa27003cf, 0x2354, 0x4f2a, 0x8d, 0x6a, 0xab, 0x7c, 0xff, 0x15, 0x43, 0x7e };

#ifndef MFASYNC_CALLBACK_QUEUE_STANDARD
#define MFASYNC_CALLBACK_QUEUE_STANDARD 0x00000001
#endif

class CStartCallback : public IMFAsyncCallback
{
public:
    CStartCallback() : m_ref(1), m_hr(S_OK), m_hEvent(nullptr)
    {
        m_hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    }

    // IUnknown
    HRESULT QueryInterface(REFIID riid, void** ppv) override
    {
        if (ppv == nullptr) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IMFAsyncCallback) {
            *ppv = static_cast<IMFAsyncCallback*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG AddRef() override { return InterlockedIncrement((LONG*)&m_ref); }
    ULONG Release() override { ULONG r = InterlockedDecrement((LONG*)&m_ref); if (r == 0) { if (m_hEvent) CloseHandle(m_hEvent); delete this; } return r; }

    // IMFAsyncCallback
    HRESULT GetParameters(DWORD* pdwFlags, DWORD* pdwQueue) override
    {
        if (pdwFlags) *pdwFlags = 0;
        if (pdwQueue) *pdwQueue = MFASYNC_CALLBACK_QUEUE_STANDARD;
        return S_OK;
    }
    HRESULT Invoke(IMFAsyncResult* pAsyncResult) override
    {
        m_hr = pAsyncResult ? pAsyncResult->GetStatus() : S_OK;
        SetEvent(m_hEvent);
        return S_OK;
    }

    HRESULT m_hr;
    HANDLE m_hEvent;

private:
    volatile LONG m_ref;
    ~CStartCallback() { if (m_hEvent) CloseHandle(m_hEvent); }
};

static void PrintUsage()
{
    wprintf(L"Usage: Registrar.exe <add|stop|remove> [cameraName]\n");
    wprintf(L"  add     - register and start the virtual camera\n");
    wprintf(L"  stop    - stop the virtual camera\n");
    wprintf(L"  remove  - remove the virtual camera from the system\n");
}

int wmain(int argc, wchar_t* argv[])
{
    if (argc < 2) { PrintUsage(); return 1; }

    const wchar_t* action = argv[1];
    const wchar_t* cameraName = argc >= 3 ? argv[2] : L"VCam";
    bool hold = (argc >= 4) && (wcscmp(argv[3], L"hold") == 0);

    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr)) { wprintf(L"CoInitializeEx failed: 0x%08X\n", hr); return 1; }

    hr = MFStartup(MF_VERSION);
    if (FAILED(hr)) { wprintf(L"MFStartup failed: 0x%08X\n", hr); CoUninitialize(); return 1; }

    wchar_t sourceId[64];
    GuidToStringW(CLSID_VCamMediaSource, sourceId, 64);

    IMFVirtualCamera* pVCam = nullptr;
    hr = MFCreateVirtualCamera(
        MFVirtualCameraType_SoftwareCameraSource,
        MFVirtualCameraLifetime_Session,
        MFVirtualCameraAccess_CurrentUser,
        cameraName,
        sourceId,
        nullptr,
        0,
        &pVCam
    );
    if (FAILED(hr)) {
        wprintf(L"MFCreateVirtualCamera failed: 0x%08X\n", hr);
        MFShutdown();
        CoUninitialize();
        return 1;
    }

    hr = S_OK;
    if (wcscmp(action, L"add") == 0) {
        hr = pVCam->Start(nullptr);
        if (SUCCEEDED(hr)) {
            wprintf(L"Camera started (async; no Shutdown on add, matching reference).\n");
        } else {
            wprintf(L"Failed to start camera: 0x%08X\n", hr);
        }
        if (SUCCEEDED(hr) && hold) {
            wprintf(L"Holder: keeping camera alive (kill process to stop; will NOT remove on exit).\n");
            fflush(stdout);
            for (;;) {
                Sleep(5000);
                wprintf(L"[holder alive]\n");
                fflush(stdout);
            }
        }
    }
    else if (wcscmp(action, L"stop") == 0) {
        hr = pVCam->Stop();
        if (SUCCEEDED(hr)) wprintf(L"Camera stopped.\n");
        else wprintf(L"Failed to stop camera: 0x%08X\n", hr);
    }
    else if (wcscmp(action, L"remove") == 0) {
        hr = pVCam->Remove();
        if (SUCCEEDED(hr)) wprintf(L"Camera removed.\n");
        else wprintf(L"Failed to remove camera: 0x%08X\n", hr);
    }
    else {
        PrintUsage();
        pVCam->Release();
        MFShutdown();
        CoUninitialize();
        return 1;
    }

    if (wcscmp(action, L"add") != 0) {
        pVCam->Shutdown();
    }
    pVCam->Release();
    MFShutdown();
    CoUninitialize();
    return SUCCEEDED(hr) ? 0 : 1;
}
