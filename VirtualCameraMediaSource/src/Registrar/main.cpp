#include <windows.h>
#include <combaseapi.h>
#include <mfapi.h>
#include <mfvirtualcamera.h>
#include <atlbase.h>
#include <cstdio>
#include <cwchar>
#include <new>
#include "../Common/GUIDs.h"
#include "../Common/SharedMemoryContract.h"

static constexpr GUID KSCATEGORY_VIDEO_CAMERA =
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
    wprintf(L"Usage: Registrar.exe <add|stop|remove> [cameraName] [hold|hold-watch]\n");
    wprintf(L"  add     - register and start the virtual camera\n");
    wprintf(L"  hold    - keep the camera alive until the process is killed (manual/dev)\n");
    wprintf(L"  hold-watch - like hold, but self-exits when there is no writer AND no\n");
    wprintf(L"              consumer for kWatchIdleMs (used by the tray host)\n");
    wprintf(L"  stop    - stop the virtual camera\n");
    wprintf(L"  remove  - remove the virtual camera from the system\n");
}

// hold-watch: живём, пока есть писатель (seq секции двигается) ИЛИ активный
// потребитель (heartbeat readerLastActiveTick свежий). Оба мертвы kWatchIdleMs
// подряд → выход → камера сама исчезает из списка устройств (Session lifetime).
// Обычный hold (register.bat/README/e2e) остаётся вечным — поведение не меняется.
static int HoldWatchLoop()
{
    constexpr ULONGLONG kCheckMs = 5000;
    constexpr ULONGLONG kWatchIdleMs = 30000;

    const wchar_t* prefixes[] = { L"Global\\", L"Local\\" };
    const wchar_t* base = wcschr(vcam::VCamSectionName, L'\\');
    base = base ? base + 1 : vcam::VCamSectionName;

    HANDLE hSection = nullptr;
    void* pView = nullptr;
    ULONGLONG seqChangedAt = GetTickCount64();      // «writer жив» отсюда
    ULONGLONG consumerActiveAt = GetTickCount64();  // «потребитель жив» отсюда
    LONGLONG lastSeq = 0;
    bool haveSeq = false;
    bool openedOnce = false;

    wprintf(L"Holder watch: exit when writer and consumers are idle %llu ms\n",
            kWatchIdleMs);
    fflush(stdout);

    for (;;) {
        Sleep((DWORD)kCheckMs);

        // (Re)open секции: её может не быть (писатель ещё не стартовал) или
        // она могла умереть (все handle'ы закрыты) и появиться заново.
        if (!hSection) {
            for (const wchar_t* pre : prefixes) {
                wchar_t name[MAX_PATH] = {};
                swprintf_s(name, L"%s%s", pre, base);
                hSection = OpenFileMappingW(FILE_MAP_READ, FALSE, name);
                if (hSection) break;
            }
            if (hSection) {
                pView = MapViewOfFile(hSection, FILE_MAP_READ, 0, 0,
                                      sizeof(vcam::VCamSectionHeader));
                if (!pView) { CloseHandle(hSection); hSection = nullptr; }
                else {
                    auto* hdr = static_cast<vcam::VCamSectionHeader*>(pView);
                    if (hdr->magic == vcam::VCamMagic) {
                        lastSeq = hdr->seq;
                        haveSeq = true;
                    }
                    openedOnce = true;
                }
            }
        }

        ULONGLONG now = GetTickCount64();

        // Писатель жив: seq продвинулся с прошлой проверки.
        if (pView) {
            auto* hdr = static_cast<vcam::VCamSectionHeader*>(pView);
            if (hdr->magic == vcam::VCamMagic) {
                LONGLONG seq = hdr->seq;
                if (!haveSeq || seq != lastSeq) {
                    lastSeq = seq;
                    haveSeq = true;
                    seqChangedAt = now;
                }
                // Потребитель жив: heartbeat свежий.
                if (hdr->readerLastActiveTick != 0 &&
                    now >= hdr->readerLastActiveTick &&
                    now - hdr->readerLastActiveTick <= 3000) {
                    consumerActiveAt = now;
                }
            }
            else {
                haveSeq = false; // секция переинициализирована — ждём seq заново
            }
        }

        ULONGLONG writerIdle = now - seqChangedAt;
        ULONGLONG consumerIdle = now - consumerActiveAt;
        wprintf(L"[holder alive] writerIdle=%llums consumerIdle=%llums\n",
                writerIdle, consumerIdle);
        fflush(stdout);

        // Секцию так и не удалось открыть — не уходим наугад, держим камеру
        // (эквивалент старого hold; unregister/rem снаружи всё равно доступны).
        if (!openedOnce) continue;

        if (writerIdle >= kWatchIdleMs && consumerIdle >= kWatchIdleMs) {
            wprintf(L"Holder watch: writer and consumers idle %llu ms - exiting,"
                    L" camera will be removed\n", kWatchIdleMs);
            fflush(stdout);
            if (pView) UnmapViewOfFile(pView);
            if (hSection) CloseHandle(hSection);
            return 0; // Session lifetime: выход процесса = камера исчезла
        }
    }
}

int wmain(int argc, wchar_t* argv[])
{
    if (argc < 2) { PrintUsage(); return 1; }

    const wchar_t* action = argv[1];
    const wchar_t* cameraName = argc >= 3 ? argv[2] : L"VCam";
    bool hold = (argc >= 4) && (wcscmp(argv[3], L"hold") == 0);
    bool holdWatch = (argc >= 4) && (wcscmp(argv[3], L"hold-watch") == 0);

    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr)) { wprintf(L"CoInitializeEx failed: 0x%08X\n", hr); return 1; }

    hr = MFStartup(MF_VERSION);
    if (FAILED(hr)) { wprintf(L"MFStartup failed: 0x%08X\n", hr); CoUninitialize(); return 1; }

    wchar_t sourceId[64];
    GuidToStringW(CLSID_VCamMediaSource, sourceId, 64);

    ATL::CComPtr<IMFVirtualCamera> pVCam;
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
        pVCam = nullptr;
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
        if (SUCCEEDED(hr) && holdWatch) {
            // Референс держим весь цикл (как hold): удержание камеры — сам
            // процесс; hold-watch уходит, когда писатель и потребители мертвы.
            int rc = HoldWatchLoop();
            pVCam = nullptr;
            MFShutdown();
            CoUninitialize();
            return rc;
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
        pVCam = nullptr;
        MFShutdown();
        CoUninitialize();
        return 1;
    }

    if (wcscmp(action, L"add") != 0) {
        pVCam->Shutdown();
    }
    pVCam = nullptr;
    MFShutdown();
    CoUninitialize();
    return SUCCEEDED(hr) ? 0 : 1;
}
