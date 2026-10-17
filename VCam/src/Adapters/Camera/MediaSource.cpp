#define INITGUID
#include <windows.h>
#include <cguid.h>
#include <cstdio>
#include "MediaSource.h"
#include "MediaStream.h"
#include <Mferror.h>

// TEMP DIAGNOSTIC - defined in dllmain.cpp, remove before shipping
void VCamDiagLog(const wchar_t* fmt, ...);
void VCamDiagGuid(const GUID* g, wchar_t* out, size_t cch);

// The local SDK's hstring.h (winrt\) declares the HSTRING type but not the factory.
extern "C" HRESULT WINAPI WindowsCreateString(PCWSTR sourceString, UINT32 length, HSTRING* resultString);

// MF_VIRTUALCAMERA_PROVIDE_ASSOCIATED_CAMERA_SOURCES (mfvirtualcamera.h).
// The FrameServer reads this UINT32 and faults (RPC_E_SERVERFAULT) when it is
// missing; this camera provides no associated camera sources, so the value is 0.
static constexpr GUID kVcamProvideAssociatedCameraSources =
    { 0xf0273718, 0x4a4d, 0x4ac5, { 0xa1, 0x5d, 0x30, 0x5e, 0xb5, 0xe9, 0x06, 0x67 } };

// Canonical IMFMediaEventGenerator IID (local SDK value differs)
static const IID kIID_IMFMediaEventGenerator_Canonical = { 0x1868091e, 0xab5a, 0x415f, { 0xa0, 0x2f, 0x5c, 0x4d, 0xd0, 0xcf, 0x90, 0x1d } };

CMediaSource::CMediaSource()
{
    VCamObjectInc();
}

CMediaSource::~CMediaSource()
{
    VCamDiagLog(L"Src.~dtor");
    m_pStream = nullptr;
    m_pEventQueue = nullptr;
    m_pSourceAttrsProxy = nullptr;
    m_pSourceAttrs = nullptr;
    VCamObjectDec();
}

HRESULT CMediaSource::QueryInterface(REFIID riid, void** ppvObject)
{
    if (ppvObject == nullptr) return E_POINTER;
    HRESULT hr = E_NOINTERFACE;
    // Each interface gets its own properly-adjusted subobject pointer
    // (reinterpret_cast would not account for the multiple-inheritance offsets).
    if (riid == IID_IUnknown) {
        *ppvObject = static_cast<IMFMediaSource2*>(this);
        AddRef();
        hr = S_OK;
    }
    else if (riid == IID_IMFMediaEventGenerator ||
        riid == kIID_IMFMediaEventGenerator_Canonical) {
        *ppvObject = static_cast<IMFMediaEventGenerator*>(this);
        AddRef();
        hr = S_OK;
    }
    else if (riid == IID_IMFMediaSource) {
        *ppvObject = static_cast<IMFMediaSource*>(this);
        AddRef();
        hr = S_OK;
    }
    else if (riid == IID_IMFMediaSourceEx) {
        *ppvObject = static_cast<IMFMediaSourceEx*>(this);
        AddRef();
        hr = S_OK;
    }
    else if (riid == IID_IMFMediaSource2) {
        *ppvObject = static_cast<IMFMediaSource2*>(this);
        AddRef();
        hr = S_OK;
    }
    else if (riid == IID_IMFGetService) {
        *ppvObject = static_cast<IMFGetService*>(this);
        AddRef();
        hr = S_OK;
    }
    else if (riid == IID_IMFRealTimeClientEx) {
        *ppvObject = static_cast<IMFRealTimeClientEx*>(this);
        AddRef();
        hr = S_OK;
    }
    else if (riid == IID_IKsControl) {
        *ppvObject = static_cast<IKsControl*>(this);
        AddRef();
        hr = S_OK;
    }
    else if (riid == IID_IAMVideoProcAmp) {
        // Тонкий прокси поверх pipe-канала (Sub 2): время жизни — внешник.
        *ppvObject = static_cast<IAMVideoProcAmp*>(&m_procAmpProxy);
        AddRef();
        hr = S_OK;
    }
    else if (riid == IID_IAMCameraControl) {
        *ppvObject = static_cast<IAMCameraControl*>(&m_camProxy);
        AddRef();
        hr = S_OK;
    }
    else if (riid == IID_IInspectable ||
        riid == kIID_IInspectable_Canonical) {
        *ppvObject = static_cast<IInspectable*>(this);
        AddRef();
        hr = S_OK;
    }
    else if (riid == __uuidof(IMFSampleAllocatorControl)) {
        *ppvObject = static_cast<IMFSampleAllocatorControl*>(this);
        AddRef();
        hr = S_OK;
    }
    else if (riid == IID_IMFAttributes ||
        riid == kIID_IMFAttributes_Canonical) {
        // Debug proxy when present, the real store otherwise (release builds).
        IMFAttributes* pAttrs = (m_pSourceAttrsProxy.p != nullptr)
            ? static_cast<IMFAttributes*>(m_pSourceAttrsProxy.p) : m_pSourceAttrs.p;
        if (pAttrs != nullptr) {
            hr = pAttrs->QueryInterface(riid, ppvObject);
        }
        else {
            *ppvObject = nullptr;
        }
    }
    else {
        *ppvObject = nullptr;
    }
    wchar_t guidStr[64];
    VCamDiagGuid(&riid, guidStr, 64);
    VCamDiagLog(L"CMediaSource::QI %s -> 0x%08X", guidStr, hr);
    return hr;
}

STDAPI_(ULONG) CMediaSource::AddRef()
{
    return InternalAddRef();
}

STDAPI_(ULONG) CMediaSource::Release()
{
    ULONG ref = InternalRelease();
    if (ref == 0) delete this;
    return ref;
}

// IInspectable
STDMETHODIMP CMediaSource::GetIids(ULONG* iidCount, IID** iids)
{
    if (iidCount == nullptr) return E_POINTER;
    static const IID s_iids[] = {
        __uuidof(IUnknown),
        __uuidof(IMFMediaEventGenerator),
        __uuidof(IMFMediaSource),
        __uuidof(IMFMediaSourceEx),
        __uuidof(IMFMediaSource2),
        __uuidof(IMFGetService),
        __uuidof(IMFRealTimeClientEx),
        IID_IKsControl,
        __uuidof(IAMVideoProcAmp),
        __uuidof(IAMCameraControl),
        __uuidof(IInspectable),
    };
    const UINT count = _countof(s_iids);
    if (iids == nullptr) {
        *iidCount = count;
        return S_OK;
    }
    UINT n = *iidCount < count ? *iidCount : count;
    for (UINT i = 0; i < n; ++i) iids[i] = const_cast<IID*>(&s_iids[i]);
    *iidCount = count;
    return S_OK;
}

STDMETHODIMP CMediaSource::GetRuntimeClassName(HSTRING* className)
{
    if (className == nullptr) return E_POINTER;
    return WindowsCreateString(L"VirtualCameraMediaSource.MediaSource", 0, className);
}

STDMETHODIMP CMediaSource::GetTrustLevel(TrustLevel* trustLevel)
{
    if (trustLevel == nullptr) return E_POINTER;
    *trustLevel = BaseTrust;
    return S_OK;
}

// IMFSampleAllocatorControl
STDMETHODIMP CMediaSource::SetDefaultAllocator(DWORD dwOutputStreamID, IUnknown* pAllocator)
{
    VCamDiagLog(L"Src.SetDefaultAllocator id=%u", dwOutputStreamID);
    if (pAllocator == nullptr) return E_POINTER;
    if (m_pStream == nullptr) return E_FAIL;
    return m_pStream->SetAllocator(pAllocator);
}

STDMETHODIMP CMediaSource::GetAllocatorUsage(DWORD dwOutputStreamID, DWORD* pdwInputStreamID, MFSampleAllocatorUsage* peUsage)
{
    if (peUsage == nullptr || pdwInputStreamID == nullptr) return E_POINTER;
    if (m_pStream == nullptr) return E_FAIL;
    *pdwInputStreamID = dwOutputStreamID;
    *peUsage = MFSampleAllocatorUsage_UsesProvidedAllocator;
    return S_OK;
}

// IMFMediaEventGenerator
HRESULT CMediaSource::GetEvent(DWORD dwFlags, IMFMediaEvent** ppEvent)
{
    VCamDiagLog(L"Src.GetEvent dwFlags=0x%X", dwFlags);
    if (m_shutdown) return MF_E_SHUTDOWN;
    if (m_pEventQueue == nullptr) return MF_E_SHUTDOWN;
    HRESULT hr = m_pEventQueue->GetEvent(dwFlags, ppEvent);
    VCamDiagLog(L"Src.GetEvent -> 0x%08X", hr);
    return hr;
}

HRESULT CMediaSource::BeginGetEvent(IMFAsyncCallback* pCallback, IUnknown* punkState)
{
    VCamDiagLog(L"Src.BeginGetEvent");
    if (m_shutdown) return MF_E_SHUTDOWN;
    if (m_pEventQueue == nullptr) return MF_E_SHUTDOWN;
    return m_pEventQueue->BeginGetEvent(pCallback, punkState);
}

HRESULT CMediaSource::EndGetEvent(IMFAsyncResult* pResult, IMFMediaEvent** ppEvent)
{
    VCamDiagLog(L"Src.EndGetEvent");
    if (m_shutdown) return MF_E_SHUTDOWN;
    if (m_pEventQueue == nullptr) return MF_E_SHUTDOWN;
    return m_pEventQueue->EndGetEvent(pResult, ppEvent);
}

HRESULT CMediaSource::QueueEvent(MediaEventType met, REFGUID guidExtendedType, HRESULT hrStatus, const PROPVARIANT* pvValue)
{
    if (m_shutdown) return MF_E_SHUTDOWN;
    if (m_pEventQueue == nullptr) return MF_E_SHUTDOWN;
    return m_pEventQueue->QueueEventParamVar(met, guidExtendedType, hrStatus, pvValue);
}

// IMFMediaSource
HRESULT CMediaSource::GetCharacteristics(DWORD* pdwCharacteristics)
{
    VCamDiagLog(L"Src.GetCharacteristics");
    if (pdwCharacteristics == nullptr) return E_POINTER;
    if (m_shutdown) return MF_E_SHUTDOWN;
    *pdwCharacteristics = m_characteristics;
    return S_OK;
}

HRESULT CMediaSource::CreatePresentationDescriptor(IMFPresentationDescriptor** ppDesc)
{
    VCamDiagLog(L"Src.CreatePresentationDescriptor");
    if (ppDesc == nullptr) return E_POINTER;
    if (m_shutdown) return MF_E_SHUTDOWN;
    ATL::CComPtr<IMFStreamDescriptor> pSd;
    HRESULT hr = m_pStream->GetStreamDescriptor(&pSd);
    if (FAILED(hr)) return hr;
    hr = MFCreatePresentationDescriptor(1, &pSd.p, ppDesc);
    pSd = nullptr;
    if (SUCCEEDED(hr)) {
        hr = (*ppDesc)->SelectStream(0);
        if (FAILED(hr)) {
            (*ppDesc)->Release();
            *ppDesc = nullptr;
        }
    }
    return hr;
}

HRESULT CMediaSource::Start(IMFPresentationDescriptor* pPresentationDescriptor, const GUID* pguidTimeFormat, const PROPVARIANT* pvarStartPosition)
{
    VCamDiagLog(L"Src.Start ptd=%p vt=%p", (const void*)pPresentationDescriptor, (const void*)pvarStartPosition);
    if (m_shutdown) return MF_E_SHUTDOWN;
    if (pPresentationDescriptor == nullptr || pvarStartPosition == nullptr) return E_INVALIDARG;
    if (pguidTimeFormat != nullptr && *pguidTimeFormat != GUID_NULL) return MF_E_UNSUPPORTED_TIME_FORMAT;
    if (pvarStartPosition->vt != VT_EMPTY && pvarStartPosition->vt != VT_I8) return MF_E_UNSUPPORTED_TIME_FORMAT;
    if (pvarStartPosition->vt == VT_I8 && pvarStartPosition->hVal.QuadPart != 0) return MF_E_INVALIDREQUEST;
    if (m_started) {
        MF_STREAM_STATE st = MF_STREAM_STATE_STOPPED;
        if (m_pStream != nullptr && SUCCEEDED(m_pStream->GetStreamState(&st)) &&
            st == MF_STREAM_STATE_RUNNING) {
            return MF_E_INVALID_STATE_TRANSITION;
        }
        VCamDiagLog(L"Src.Start restart after stream stopped without Src.Stop");
        m_started = false;
    }

    // Verify stream 0 is selected
    BOOL selected = FALSE;
    ATL::CComPtr<IMFStreamDescriptor> pSd;
    HRESULT hr = pPresentationDescriptor->GetStreamDescriptorByIndex(0, &selected, &pSd);
    if (SUCCEEDED(hr)) pSd = nullptr;
    if (FAILED(hr) || !selected) return MF_E_INVALIDREQUEST;

    m_started = true;

    // 1. Start the stream (queues MEStreamStarted on stream queue)
    hr = m_pStream->StartForSession();
    if (FAILED(hr)) {
        m_started = false;
        PROPVARIANT vtErr;
        PropVariantInit(&vtErr);
        QueueEvent(MESourceStarted, GUID_NULL, hr, &vtErr);
        PropVariantClear(&vtErr);
        return hr;
    }

    // 2. Queue MENewStream (stream as IUnknown) on source queue
    ATL::CComPtr<IUnknown> pStreamUnknown;
    hr = m_pStream->QueryInterface(IID_PPV_ARGS(&pStreamUnknown));
    if (FAILED(hr) || pStreamUnknown == nullptr) {
        PROPVARIANT vtErr;
        PropVariantInit(&vtErr);
        QueueEvent(MESourceStarted, GUID_NULL, hr, &vtErr);
        PropVariantClear(&vtErr);
        return hr;
    }
    PROPVARIANT vtNewStream;
    PropVariantInit(&vtNewStream);
    vtNewStream.vt = VT_UNKNOWN;
    vtNewStream.punkVal = pStreamUnknown.Detach(); // transfer; PropVariantClear releases
    hr = QueueEvent(MENewStream, GUID_NULL, S_OK, &vtNewStream);
    PropVariantClear(&vtNewStream);
    if (FAILED(hr)) {
        m_pStream->StopForSession();
        m_started = false;
        PROPVARIANT vtErr;
        PropVariantInit(&vtErr);
        QueueEvent(MESourceStarted, GUID_NULL, hr, &vtErr);
        PropVariantClear(&vtErr);
        return hr;
    }

    // 3. Queue MESourceStarted (last)
    HRESULT hrStart = S_OK;
    if (pvarStartPosition->vt == VT_EMPTY) {
        PROPVARIANT vtActual;
        PropVariantInit(&vtActual);
        vtActual.vt = VT_I8;
        vtActual.hVal.QuadPart = MFGetSystemTime();
        hrStart = QueueEvent(MESourceStarted, GUID_NULL, S_OK, &vtActual);
        PropVariantClear(&vtActual);
    } else {
        hrStart = QueueEvent(MESourceStarted, GUID_NULL, S_OK, pvarStartPosition);
    }
    if (FAILED(hrStart)) {
        m_pStream->StopForSession();
        m_started = false;
    }
    return hrStart;
}

HRESULT CMediaSource::Stop()
{
    VCamDiagLog(L"Src.Stop");
    if (m_shutdown) return MF_E_SHUTDOWN;
    if (!m_started) return S_OK;
    m_started = false;

    HRESULT hr = m_pStream->StopForSession();
    HRESULT hr2 = QueueEvent(MESourceStopped, GUID_NULL, S_OK, nullptr);
    return SUCCEEDED(hr) ? hr2 : hr;
}

HRESULT CMediaSource::Pause()
{
    VCamDiagLog(L"Src.Pause");
    if (m_shutdown) return MF_E_SHUTDOWN;
    return MF_E_INVALID_STATE_TRANSITION;
}

HRESULT CMediaSource::Shutdown()
{
    VCamDiagLog(L"Src.Shutdown");
    if (m_shutdown) return MF_E_SHUTDOWN;
    m_shutdown = true;

    if (m_pStream) {
        m_pStream->ShutDownInternal();
        m_pStream = nullptr;
    }
    QueueEvent(MEError, GUID_NULL, MF_E_SHUTDOWN, nullptr);

    m_pEventQueue = nullptr;
    m_pSourceAttrsProxy = nullptr;
    m_pSourceAttrs = nullptr;
    return S_OK;
}

// IMFMediaSourceEx
HRESULT CMediaSource::GetSourceAttributes(IMFAttributes** ppAttributes)
{
    VCamDiagLog(L"Src.GetSourceAttributes");
    if (ppAttributes == nullptr) return E_POINTER;
    if (m_shutdown) return MF_E_SHUTDOWN;
    *ppAttributes = (m_pSourceAttrsProxy.p != nullptr) ? static_cast<IMFAttributes*>(m_pSourceAttrsProxy.p) : m_pSourceAttrs.p;
    (*ppAttributes)->AddRef();
    return S_OK;
}

HRESULT CMediaSource::CopyActivationAttributes(IMFAttributes* pActivatorAttrs)
{
    if (pActivatorAttrs == nullptr || m_pSourceAttrs == nullptr) return E_POINTER;
    VCamDiagLog(L"Src.CopyActivationAttributes");
    HRESULT hr = pActivatorAttrs->CopyAllItems(m_pSourceAttrs);
    VCamDiagLog(L"Src.CopyActivationAttributes -> 0x%08X", hr);
    return hr;
}

HRESULT CMediaSource::GetStreamAttributes(DWORD dwStreamIdentifier, IMFAttributes** ppAttributes)
{
    VCamDiagLog(L"Src.GetStreamAttributes id=%u", dwStreamIdentifier);
    if (ppAttributes == nullptr) return E_POINTER;
    if (m_shutdown) return MF_E_SHUTDOWN;
    if (dwStreamIdentifier != 0) return MF_E_INVALIDSTREAMNUMBER;
    if (m_pStream == nullptr) return MF_E_SHUTDOWN;
    // Borrowed pointer; the stream owns the attributes.
    *ppAttributes = m_pStream->GetStreamAttributes();
    return S_OK;
}

// IMFMediaSource2
HRESULT CMediaSource::SetMediaType(DWORD dwStreamID, IMFMediaType* pMediaType)
{
    VCamDiagLog(L"Src.SetMediaType id=%u", dwStreamID);
    if (m_shutdown) return MF_E_SHUTDOWN;
    if (pMediaType == nullptr) return E_POINTER;
    if (dwStreamID != 0) return MF_E_INVALIDSTREAMNUMBER;
    if (m_pStream == nullptr) return MF_E_SHUTDOWN;
    return m_pStream->SetMediaType(pMediaType);
}

HRESULT CMediaSource::GetStreamByStreamID(LPGUID pguidStreamID, IMFMediaStream** ppStream)
{
    VCamDiagLog(L"Src.GetStreamByStreamID");
    if (ppStream == nullptr) return E_POINTER;
    *ppStream = nullptr;
    if (pguidStreamID == nullptr) return E_POINTER;
    if (m_shutdown) return MF_E_SHUTDOWN;
    if (m_pStream == nullptr) return MF_E_NOT_FOUND;
    if (*pguidStreamID != VCAM_VIDEO_STREAM_ID && *pguidStreamID != GUID_NULL) return MF_E_NOT_FOUND;
    return m_pStream->QueryInterface(IID_PPV_ARGS(ppStream));
}

HRESULT CMediaSource::SetD3DManager(IUnknown* pManager)
{
    VCamDiagLog(L"Src.SetD3DManager");
    if (m_shutdown) return MF_E_SHUTDOWN;
    return S_OK;
}

// IMFGetService
HRESULT CMediaSource::GetService(REFGUID rSID, REFIID riid, void** ppv)
{
    wchar_t sidStr[64];
    wchar_t riidStr[64];
    VCamDiagGuid(&rSID, sidStr, 64);
    VCamDiagGuid(&riid, riidStr, 64);
    VCamDiagLog(L"Src.GetService sid=%s riid=%s", sidStr, riidStr);
    if (ppv == nullptr) return E_POINTER;
    *ppv = nullptr;
    // Реальные клиенты (Zoom и т.п.) просят IAM-контролы через GetService
    // с нулевым/любым rSID — отдаём прокси (Sub 2). Остальное — как было.
    if (riid == IID_IAMVideoProcAmp || riid == IID_IAMCameraControl)
        return QueryInterface(riid, ppv);
    return MF_E_UNSUPPORTED_SERVICE;
}

// IMFRealTimeClientEx - we run our own worker thread; the host only needs
// registration to succeed without claiming a task index.
HRESULT CMediaSource::RegisterThreadsEx(DWORD* pdwTaskIndex, LPCWSTR wszClassName, LONG lBasePriority)
{
    VCamDiagLog(L"Src.RegisterThreadsEx cls=%s basePri=%d", wszClassName ? wszClassName : L"(null)", lBasePriority);
    if (pdwTaskIndex == nullptr) return E_POINTER;
    *pdwTaskIndex = 0;
    return S_OK;
}

HRESULT CMediaSource::UnregisterThreads()
{
    VCamDiagLog(L"Src.UnregisterThreads");
    return S_OK;
}

HRESULT CMediaSource::SetWorkQueueEx(DWORD dwMultithreadedWorkQueueId, LONG lWorkItemBasePriority)
{
    VCamDiagLog(L"Src.SetWorkQueueEx wq=%u pri=%d", dwMultithreadedWorkQueueId, lWorkItemBasePriority);
    return S_OK;
}

// IKsControl stubs (no KS properties exposed)
HRESULT CMediaSource::KsProperty(PKSPROPERTY Property, ULONG PropertyLength, LPVOID PropertyData, ULONG DataLength, ULONG* BytesReturned)
{
    wchar_t guidStr[64];
    if (Property != nullptr) {
        VCamDiagGuid(&Property->Set, guidStr, 64);
        VCamDiagLog(L"Src.KsProperty set=%s id=%u -> ERROR_SET_NOT_FOUND", guidStr, Property->Id);
    }
    else VCamDiagLog(L"Src.KsProperty (null) -> ERROR_SET_NOT_FOUND");
    return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}

HRESULT CMediaSource::KsMethod(PKSMETHOD Method, ULONG MethodLength, LPVOID MethodData, ULONG DataLength, ULONG* BytesReturned)
{
    wchar_t guidStr[64];
    if (Method != nullptr) {
        VCamDiagGuid(&Method->Set, guidStr, 64);
        VCamDiagLog(L"Src.KsMethod set=%s id=%u -> ERROR_SET_NOT_FOUND", guidStr, Method->Id);
    }
    else VCamDiagLog(L"Src.KsMethod (null) -> ERROR_SET_NOT_FOUND");
    return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}

HRESULT CMediaSource::KsEvent(PKSEVENT Event, ULONG EventLength, LPVOID EventData, ULONG DataLength, ULONG* BytesReturned)
{
    wchar_t guidStr[64];
    if (Event != nullptr) {
        VCamDiagGuid(&Event->Set, guidStr, 64);
        VCamDiagLog(L"Src.KsEvent set=%s id=%u -> ERROR_SET_NOT_FOUND", guidStr, Event->Id);
    }
    else VCamDiagLog(L"Src.KsEvent (null) -> ERROR_SET_NOT_FOUND");
    return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}

HRESULT CMediaSource::FinalConstruct()
{
    VCamDiagLog(L"Src.FinalConstruct");
    // Канонический IUnknown для IAM-прокси (делегирование ссылок/QI).
    ATL::CComPtr<IUnknown> pUnk;
    HRESULT hrInit = QueryInterface(IID_PPV_ARGS(&pUnk));
    if (SUCCEEDED(hrInit) && pUnk != nullptr) {
        m_procAmpProxy.Init(pUnk);
        m_camProxy.Init(pUnk);
    }
    HRESULT hr = MFCreateEventQueue(&m_pEventQueue);
    if (FAILED(hr)) return hr;

    hr = MFCreateAttributes(&m_pSourceAttrs, 8);
    if (FAILED(hr)) return hr;

#ifndef NDEBUG // TEMP DIAGNOSTIC: attribute-access proxy is debug-only
    m_pSourceAttrsProxy.Attach(new (std::nothrow) CAttrLogProxy(m_pSourceAttrs, L"Src"));
    if (m_pSourceAttrsProxy == nullptr) return E_OUTOFMEMORY;
#endif

    ATL::CComPtr<CMediaStream> pStream;
    pStream.Attach(new (std::nothrow) CMediaStream());
    if (pStream == nullptr) return E_OUTOFMEMORY;
    pStream.p->AddRef(); // one local ref; transferred into m_pStream below
    hr = pStream->FinalConstruct(this);
    if (FAILED(hr)) {
        return hr; // pStream dtor releases (was pStream->Release())
    }
    m_pStream = std::move(pStream); // transfer the owned ref

    return S_OK;
}

HRESULT CMediaSource::SetIntrinsicAttributes()
{
    VCamDiagLog(L"Src.SetIntrinsicAttributes");
    if (m_pSourceAttrs == nullptr) return E_POINTER;
    HRESULT hr = E_FAIL;
    // MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE is a GUID per MSDN (not a UINT32);
    // a video camera reports MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID.
    hr = m_pSourceAttrs->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
                                  MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    if (FAILED(hr)) return hr;
    // The FrameServer reads MF_VIRTUALCAMERA_PROVIDE_ASSOCIATED_CAMERA_SOURCES
    // (UINT32) and faults if it is missing. We provide none -> 0.
    hr = m_pSourceAttrs->SetUINT32(kVcamProvideAssociatedCameraSources, 0);
    if (FAILED(hr)) return hr;

    // Reference parity: identify the class behind the device.
    hr = m_pSourceAttrs->SetGUID(kMftTransformClsidAttribute, CLSID_VCamMediaSource);
    if (FAILED(hr)) return hr;

    // Reference parity: MF_DEVICEMFT_SENSORPROFILE_COLLECTION — camera
    // profiles the capture engine enumerates (Legacy + HighFrameRate).
    ATL::CComPtr<IMFSensorProfileCollection> pProfileCollection;
    hr = MFCreateSensorProfileCollection(&pProfileCollection);
    if (FAILED(hr)) return hr;

    ATL::CComPtr<IMFSensorProfile> pProfile;
    hr = MFCreateSensorProfile(kKsCameraProfileLegacy, 0, nullptr, &pProfile);
    if (FAILED(hr)) return hr;
    hr = pProfile->AddProfileFilter(0, L"((RES==;FRT<=30,1;SUT==))");
    if (FAILED(hr)) return hr;
    hr = pProfileCollection->AddProfile(pProfile);
    pProfile = nullptr;
    if (FAILED(hr)) return hr;

    hr = MFCreateSensorProfile(kKsCameraProfileHighFrameRate, 0, nullptr, &pProfile);
    if (FAILED(hr)) return hr;
    hr = pProfile->AddProfileFilter(0, L"((RES==;FRT>=60,1;SUT==))");
    if (FAILED(hr)) return hr;
    hr = pProfileCollection->AddProfile(pProfile);
    pProfile = nullptr;
    if (FAILED(hr)) return hr;

    hr = m_pSourceAttrs->SetUnknown(MF_DEVICEMFT_SENSORPROFILE_COLLECTION, pProfileCollection);
    pProfileCollection = nullptr;
    if (FAILED(hr)) return hr;

    // 24H2 (NTDDI_WIN10_CO) flag; the frameserver may probe it
    hr = m_pSourceAttrs->SetUINT32(kMsCameraEffectsAttribute, 0);
    return hr;
}
