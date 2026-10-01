#define INITGUID
#include <windows.h>
#include <cguid.h>
#include "MediaStream.h"
#include "MediaSource.h"
#include "SharedMemoryContract.h"
#include "SharedMemoryFrameSource.h"
#include <mfapi.h>
#include <Mferror.h>

// TEMP DIAGNOSTIC - defined in dllmain.cpp, remove before shipping
void VCamDiagLog(const wchar_t* fmt, ...);
void VCamDiagGuid(const GUID* g, wchar_t* out, size_t cch);
// Always-on (release too): e2e_test.ps1 phases D/E assert these exact lines.
void VCamDiagEvent(const wchar_t* fmt, ...);

// The local SDK's hstring.h (winrt\) declares the HSTRING type but not the factory.
extern "C" HRESULT WINAPI WindowsCreateString(PCWSTR sourceString, UINT32 length, HSTRING* resultString);

// Canonical IMFMediaEventGenerator IID (local SDK value differs)
static const IID kIID_IMFMediaEventGenerator_Canonical = { 0x1868091e, 0xab5a, 0x415f, { 0xa0, 0x2f, 0x5c, 0x4d, 0xd0, 0xcf, 0x90, 0x1d } };

CMediaStream::CMediaStream()
{
    VCamObjectInc();
}

CMediaStream::~CMediaStream()
{
    VCamDiagLog(L"Stream.~dtor");
    if (m_pEventQueue) m_pEventQueue->Release();
    if (m_pStreamDescriptor) m_pStreamDescriptor->Release();
    if (m_pMediaType) m_pMediaType->Release();
    if (m_pMediaTypeNv12) m_pMediaTypeNv12->Release();
    if (m_pMediaType640) m_pMediaType640->Release();
    if (m_pNv12Scratch) { delete[] m_pNv12Scratch; m_pNv12Scratch = nullptr; }
    if (m_pStreamAttrsProxy) m_pStreamAttrsProxy->Release();
    if (m_pStreamAttributes) m_pStreamAttributes->Release();
    if (m_pAllocator) m_pAllocator->Release();
    if (m_csInit) DeleteCriticalSection(&m_cs);
    VCamObjectDec();
}

HRESULT CMediaStream::QueryInterface(REFIID riid, void** ppvObject)
{
    if (ppvObject == nullptr) return E_POINTER;
    HRESULT hr = E_NOINTERFACE;
    // Each interface gets its own properly-adjusted subobject pointer.
    if (riid == IID_IUnknown) {
        *ppvObject = static_cast<IMFMediaStream2*>(this);
        AddRef();
        hr = S_OK;
    }
    else if (riid == IID_IMFMediaEventGenerator ||
        riid == kIID_IMFMediaEventGenerator_Canonical) {
        *ppvObject = static_cast<IMFMediaEventGenerator*>(this);
        AddRef();
        hr = S_OK;
    }
    else if (riid == IID_IMFMediaStream) {
        *ppvObject = static_cast<IMFMediaStream*>(this);
        AddRef();
        hr = S_OK;
    }
    else if (riid == IID_IMFMediaStream2) {
        *ppvObject = static_cast<IMFMediaStream2*>(this);
        AddRef();
        hr = S_OK;
    }
    else if (riid == IID_IInspectable ||
        riid == kIID_IInspectable_Canonical) {
        *ppvObject = static_cast<IInspectable*>(this);
        AddRef();
        hr = S_OK;
    }
    else if (riid == IID_IMFAttributes ||
        riid == kIID_IMFAttributes_Canonical) {
        // Debug proxy when present, the real store otherwise (release builds).
        IMFAttributes* pAttrs = (m_pStreamAttrsProxy != nullptr)
            ? static_cast<IMFAttributes*>(m_pStreamAttrsProxy) : m_pStreamAttributes;
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
    VCamDiagLog(L"Stream.QI %s -> 0x%08X", guidStr, hr);
    return hr;
}

STDAPI_(ULONG) CMediaStream::AddRef()
{
    return InternalAddRef();
}

STDAPI_(ULONG) CMediaStream::Release()
{
    ULONG ref = InternalRelease();
    if (ref == 0) delete this;
    return ref;
}

// IInspectable
STDMETHODIMP CMediaStream::GetIids(ULONG* iidCount, IID** iids)
{
    if (iidCount == nullptr) return E_POINTER;
    static const IID s_iids[] = {
        __uuidof(IUnknown),
        __uuidof(IMFMediaEventGenerator),
        __uuidof(IMFMediaStream),
        __uuidof(IMFMediaStream2),
        IID_IMFAttributes,
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

STDMETHODIMP CMediaStream::GetRuntimeClassName(HSTRING* className)
{
    if (className == nullptr) return E_POINTER;
    return WindowsCreateString(L"VirtualCameraMediaSource.MediaStream", 0, className);
}

STDMETHODIMP CMediaStream::GetTrustLevel(TrustLevel* trustLevel)
{
    if (trustLevel == nullptr) return E_POINTER;
    *trustLevel = BaseTrust;
    return S_OK;
}

HRESULT CMediaStream::SetAllocator(IUnknown* pAllocator)
{
    if (pAllocator == nullptr) return E_POINTER;
    // The frameserver passes a shared (cross-session) sample allocator. QI to the
    // shared video sample allocator so AllocateSample returns buffers the consumer
    // can read across the session boundary.
    IMFVideoSampleAllocator* pShared = nullptr;
    HRESULT hr = pAllocator->QueryInterface(__uuidof(IMFVideoSampleAllocator),
                                            reinterpret_cast<void**>(&pShared));
    IUnknown* pOld = m_pAllocator;
    if (SUCCEEDED(hr)) {
        m_pAllocator = pShared;
        VCamDiagLog(L"Stream.SetAllocator shared=OK");
    }
    else {
        m_pAllocator = nullptr;
        VCamDiagLog(L"Stream.SetAllocator shared=no (0x%08X) -> local fallback", hr);
    }
    if (pOld != nullptr) pOld->Release();
    return S_OK;
}

// IMFMediaEventGenerator
HRESULT CMediaStream::GetEvent(DWORD dwFlags, IMFMediaEvent** ppEvent)
{
    VCamDiagLog(L"Stream.GetEvent dwFlags=0x%X", dwFlags);
    if (m_shutdown) return MF_E_SHUTDOWN;
    if (m_pEventQueue == nullptr) return MF_E_SHUTDOWN;
    HRESULT hr = m_pEventQueue->GetEvent(dwFlags, ppEvent);
    VCamDiagLog(L"Stream.GetEvent -> 0x%08X", hr);
    return hr;
}

HRESULT CMediaStream::BeginGetEvent(IMFAsyncCallback* pCallback, IUnknown* punkState)
{
    VCamDiagLog(L"Stream.BeginGetEvent");
    if (m_shutdown) return MF_E_SHUTDOWN;
    if (m_pEventQueue == nullptr) return MF_E_SHUTDOWN;
    return m_pEventQueue->BeginGetEvent(pCallback, punkState);
}

HRESULT CMediaStream::EndGetEvent(IMFAsyncResult* pResult, IMFMediaEvent** ppEvent)
{
    if (m_shutdown) return MF_E_SHUTDOWN;
    if (m_pEventQueue == nullptr) return MF_E_SHUTDOWN;
    return m_pEventQueue->EndGetEvent(pResult, ppEvent);
}

HRESULT CMediaStream::QueueEvent(MediaEventType met, REFGUID guidExtendedType, HRESULT hrStatus, const PROPVARIANT* pvValue)
{
    if (m_shutdown) return MF_E_SHUTDOWN;
    if (m_pEventQueue == nullptr) return MF_E_SHUTDOWN;
    return m_pEventQueue->QueueEventParamVar(met, guidExtendedType, hrStatus, pvValue);
}

// IMFMediaStream
HRESULT CMediaStream::GetMediaSource(IMFMediaSource** ppSource)
{
    VCamDiagLog(L"Stream.GetMediaSource");
    if (m_shutdown) return MF_E_SHUTDOWN;
    if (ppSource == nullptr) return E_POINTER;
    return m_pSource->QueryInterface(IID_IMFMediaSource, reinterpret_cast<void**>(ppSource));
}

HRESULT CMediaStream::GetStreamDescriptor(IMFStreamDescriptor** ppDescriptor)
{
    VCamDiagLog(L"Stream.GetStreamDescriptor");
    if (m_shutdown) return MF_E_SHUTDOWN;
    if (ppDescriptor == nullptr) return E_POINTER;
    *ppDescriptor = m_pStreamDescriptor;
    m_pStreamDescriptor->AddRef();
    return S_OK;
}

HRESULT CMediaStream::RequestSample(IUnknown* pToken)
{
    VCamDiagLog(L"Stream.RequestSample token=%p", (const void*)pToken);
    if (m_shutdown) return MF_E_SHUTDOWN;
    if (m_state != MF_STREAM_STATE_RUNNING) return MF_E_INVALIDREQUEST;

    // Reference-faithful pull model: deliver the sample SYNCHRONOUSLY on the
    // consumer's request thread. The frameserver-provided SHARED allocator's
    // AllocateSample must run on this (COM) request thread -- calling it on a
    // raw std::thread worker deadlocks -- so there is no token queue here.
    // pToken is borrowed from the caller; DeliverNextSample attaches it via
    // MFSampleExtension_Token (which AddRefs) and never releases it. A null
    // token is legal across the session proxy (reference parity: "if (pToken)").
    DeliverNextSample(pToken);
    return S_OK;
}

// IMFMediaStream2
HRESULT CMediaStream::SetStreamState(MF_STREAM_STATE newState)
{
    VCamDiagLog(L"Stream.SetStreamState %d", (int)newState);
    if (m_shutdown) return MF_E_SHUTDOWN;
    if (newState == m_state) return S_OK;
    if (newState == MF_STREAM_STATE_PAUSED) return MF_E_INVALID_STATE_TRANSITION;
    if (newState == MF_STREAM_STATE_RUNNING) return StartForSession();
    if (newState == MF_STREAM_STATE_STOPPED) return StopForSession();
    return E_INVALIDARG;
}

HRESULT CMediaStream::GetStreamState(MF_STREAM_STATE* pState)
{
    VCamDiagLog(L"Stream.GetStreamState");
    if (pState == nullptr) return E_POINTER;
    if (m_shutdown) return MF_E_SHUTDOWN;
    *pState = m_state;
    return S_OK;
}

HRESULT CMediaStream::SetMediaType(IMFMediaType* pMediaType)
{
    VCamDiagLog(L"Stream.SetMediaType");
    if (m_shutdown) return MF_E_SHUTDOWN;
    if (pMediaType == nullptr) return E_POINTER;

    // Fixed-format source: accept exact formats we produce (1280x720 or 640x480).
    GUID major = GUID_NULL, subtype = GUID_NULL;
    HRESULT hr = pMediaType->GetGUID(MF_MT_MAJOR_TYPE, &major);
    if (FAILED(hr)) return MF_E_INVALIDMEDIATYPE;
    if (major != MFMediaType_Video) return MF_E_INVALIDMEDIATYPE;

    hr = pMediaType->GetGUID(MF_MT_SUBTYPE, &subtype);
    if (FAILED(hr)) return MF_E_INVALIDMEDIATYPE;
    if (subtype != MFVideoFormat_RGB32 && subtype != MFVideoFormat_NV12) return MF_E_INVALIDMEDIATYPE;

    UINT32 w = 0, h = 0;
    hr = MFGetAttributeSize(pMediaType, MF_MT_FRAME_SIZE, &w, &h);
    if (FAILED(hr)) return MF_E_INVALIDMEDIATYPE;
    if (!((w == 1280 && h == 720) || (w == 640 && h == 480))) return MF_E_INVALIDMEDIATYPE;

    UINT32 num = 0, den = 0;
    hr = MFGetAttributeRatio(pMediaType, MF_MT_FRAME_RATE, &num, &den);
    if (SUCCEEDED(hr) && (num != 30 || den != 1)) return MF_E_INVALIDMEDIATYPE;

    m_selectedWidth = w;
    m_selectedHeight = h;
    m_selectedNv12 = (subtype == MFVideoFormat_NV12);
    VCamDiagLog(L"Stream.SetMediaType -> %ux%u %s", w, h, m_selectedNv12 ? L"NV12" : L"RGB32");

    // Keep the SD handler's current type in sync: consumers (and our own
    // ResolveNegotiatedType) may read the type from the handler, so a type set
    // through this path must be visible there too. Failure is non-fatal
    // (m_selected* already carries the negotiation), but log it.
    if (m_pStreamDescriptor != nullptr) {
        IMFMediaTypeHandler* pHandler = nullptr;
        if (SUCCEEDED(m_pStreamDescriptor->GetMediaTypeHandler(&pHandler)) && pHandler != nullptr) {
            HRESULT hrSync = pHandler->SetCurrentMediaType(pMediaType);
            VCamDiagLog(L"Stream.SetMediaType handler SetCurrentMediaType hr=0x%08X", (unsigned)hrSync);
            pHandler->Release();
        }
    }
    return S_OK;
}

HRESULT CMediaStream::FinalConstruct(CMediaSource* pSource)
{
    VCamDiagLog(L"Stream.FinalConstruct");
    m_pSource = pSource;
    if (pSource == nullptr) return E_INVALIDARG;

    InitializeCriticalSection(&m_cs);
    m_csInit = true;

    HRESULT hr = MFCreateEventQueue(&m_pEventQueue);
    if (FAILED(hr)) return hr;

    hr = MFCreateMediaType(&m_pMediaType);
    if (FAILED(hr)) return hr;

    hr = m_pMediaType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    if (FAILED(hr)) return hr;
    hr = m_pMediaType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
    if (FAILED(hr)) return hr;
    hr = MFSetAttributeSize(m_pMediaType, MF_MT_FRAME_SIZE, vcam::VCamWidth, vcam::VCamHeight);
    if (FAILED(hr)) return hr;
    hr = MFSetAttributeRatio(m_pMediaType, MF_MT_FRAME_RATE, 30, 1);
    if (FAILED(hr)) return hr;
    hr = m_pMediaType->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
    if (FAILED(hr)) return hr;
    hr = m_pMediaType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    if (FAILED(hr)) return hr;
    hr = m_pMediaType->SetUINT32(MF_MT_FIXED_SIZE_SAMPLES, TRUE);
    if (FAILED(hr)) return hr;
    hr = m_pMediaType->SetUINT32(MF_MT_DEFAULT_STRIDE, vcam::VCamStride);
    if (FAILED(hr)) return hr;
    hr = m_pMediaType->SetUINT32(MF_MT_SAMPLE_SIZE, vcam::VCamFrameSize);
    if (FAILED(hr)) return hr;
    hr = m_pMediaType->SetUINT32(MF_MT_AVG_BITRATE,
                                 (UINT32)(vcam::VCamWidth * vcam::VCamHeight * vcam::VCamPixelSize * 8 * 30));
    if (FAILED(hr)) return hr;
    hr = MFSetAttributeRatio(m_pMediaType, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    if (FAILED(hr)) return hr;

    // Secondary format: NV12 1280x720@30.
    const UINT32 nv12Bytes = (UINT32)(vcam::VCamWidth * vcam::VCamHeight * 3 / 2);
    hr = MFCreateMediaType(&m_pMediaTypeNv12);
    if (FAILED(hr)) return hr;
    hr = m_pMediaTypeNv12->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    if (FAILED(hr)) return hr;
    hr = m_pMediaTypeNv12->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    if (FAILED(hr)) return hr;
    hr = MFSetAttributeSize(m_pMediaTypeNv12, MF_MT_FRAME_SIZE, vcam::VCamWidth, vcam::VCamHeight);
    if (FAILED(hr)) return hr;
    hr = MFSetAttributeRatio(m_pMediaTypeNv12, MF_MT_FRAME_RATE, 30, 1);
    if (FAILED(hr)) return hr;
    hr = m_pMediaTypeNv12->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
    if (FAILED(hr)) return hr;
    hr = m_pMediaTypeNv12->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    if (FAILED(hr)) return hr;
    hr = m_pMediaTypeNv12->SetUINT32(MF_MT_FIXED_SIZE_SAMPLES, TRUE);
    if (FAILED(hr)) return hr;
    hr = m_pMediaTypeNv12->SetUINT32(MF_MT_DEFAULT_STRIDE, vcam::VCamWidth);
    if (FAILED(hr)) return hr;
    hr = m_pMediaTypeNv12->SetUINT32(MF_MT_SAMPLE_SIZE, nv12Bytes);
    if (FAILED(hr)) return hr;
    hr = m_pMediaTypeNv12->SetUINT32(MF_MT_AVG_BITRATE, (UINT32)(nv12Bytes * 8 * 30));
    if (FAILED(hr)) return hr;
    hr = MFSetAttributeRatio(m_pMediaTypeNv12, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    if (FAILED(hr)) return hr;

    // Tertiary format: 640x480 RGB32@30
    hr = MFCreateMediaType(&m_pMediaType640);
    if (FAILED(hr)) return hr;
    hr = m_pMediaType640->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    if (FAILED(hr)) return hr;
    hr = m_pMediaType640->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
    if (FAILED(hr)) return hr;
    hr = MFSetAttributeSize(m_pMediaType640, MF_MT_FRAME_SIZE, 640, 480);
    if (FAILED(hr)) return hr;
    hr = MFSetAttributeRatio(m_pMediaType640, MF_MT_FRAME_RATE, 30, 1);
    if (FAILED(hr)) return hr;
    hr = m_pMediaType640->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
    if (FAILED(hr)) return hr;
    hr = m_pMediaType640->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    if (FAILED(hr)) return hr;
    hr = m_pMediaType640->SetUINT32(MF_MT_FIXED_SIZE_SAMPLES, TRUE);
    if (FAILED(hr)) return hr;
    hr = m_pMediaType640->SetUINT32(MF_MT_DEFAULT_STRIDE, 640 * 4);
    if (FAILED(hr)) return hr;
    hr = m_pMediaType640->SetUINT32(MF_MT_SAMPLE_SIZE, 640 * 480 * 4);
    if (FAILED(hr)) return hr;
    hr = m_pMediaType640->SetUINT32(MF_MT_AVG_BITRATE, (UINT32)(640 * 480 * 4 * 8 * 30));
    if (FAILED(hr)) return hr;
    hr = MFSetAttributeRatio(m_pMediaType640, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    if (FAILED(hr)) return hr;

    // 640x480 NV12 намеренно НЕ предлагается: системный FrameServer-прокси
    // портит именно эту комбинацию (UV-плоскость обнуляется у клиента),
    // тогда как RGB32 640x480 и NV12 1280x720 через прокси идут чисто.

    IMFMediaType* types[3] = { m_pMediaType, m_pMediaTypeNv12, m_pMediaType640 };
    hr = MFCreateStreamDescriptor(0, 3, types, &m_pStreamDescriptor);
    if (FAILED(hr)) return hr;

    // The frameserver's Start validation calls GetCurrentMediaType on the
    // handler; without an explicit current type it fails with
    // MF_E_INVALIDTYPE (0xC00D36BD).
    IMFMediaTypeHandler* pHandler = nullptr;
    hr = m_pStreamDescriptor->GetMediaTypeHandler(&pHandler);
    if (FAILED(hr)) return hr;
    hr = pHandler->SetCurrentMediaType(m_pMediaType);
    if (FAILED(hr)) { pHandler->Release(); return hr; }
    pHandler->Release();

    hr = MFCreateAttributes(&m_pStreamAttributes, 4);
    if (FAILED(hr)) return hr;
    hr = m_pStreamAttributes->SetUINT32(MF_DEVICESTREAM_STREAM_ID, 0);
    if (FAILED(hr)) return hr;
    hr = m_pStreamAttributes->SetGUID(MF_DEVICESTREAM_STREAM_CATEGORY, VCamPinNameVideoCapture);
    if (FAILED(hr)) return hr;
    hr = m_pStreamAttributes->SetUINT32(MF_DEVICESTREAM_ATTRIBUTE_FRAMESOURCE_TYPES, MFFrameSourceTypes_Color);
    if (FAILED(hr)) return hr;
    hr = m_pStreamAttributes->SetUINT32(MF_DEVICESTREAM_FRAMESERVER_SHARED, TRUE);
    if (FAILED(hr)) return hr;

#ifndef NDEBUG // TEMP DIAGNOSTIC: attribute-access proxy is debug-only
    m_pStreamAttrsProxy = new (std::nothrow) CAttrLogProxy(m_pStreamAttributes, L"Stream");
    if (m_pStreamAttrsProxy == nullptr) return E_OUTOFMEMORY;
#endif

    // Initialize the shared memory frame source (process-wide singleton)
    hr = SharedMemoryFrameSource::Instance().Init();
    if (FAILED(hr)) return hr;

    m_state = MF_STREAM_STATE_STOPPED;
    return S_OK;
}

HRESULT CMediaStream::StartForSession()
{
    VCamDiagLog(L"Stream.StartForSession");
    if (m_shutdown) return MF_E_SHUTDOWN;
    if (m_state == MF_STREAM_STATE_RUNNING) return S_OK;

    m_state = MF_STREAM_STATE_RUNNING;
    UINT32 selW = 0, selH = 0;
    bool selNv12 = false;
    ResolveNegotiatedType(&selW, &selH, &selNv12);
    IMFMediaType* pInitType = nullptr;
    if (selNv12 && m_pMediaTypeNv12 != nullptr && selW == vcam::VCamWidth && selH == vcam::VCamHeight) {
        pInitType = m_pMediaTypeNv12;
    }
    else if (!selNv12 && m_pMediaType640 != nullptr && selW == 640 && selH == 480) {
        pInitType = m_pMediaType640;
    }
    else {
        pInitType = m_pMediaType;
    }
    VCamDiagEvent(L"Stream.StartForSession negotiated %ux%u %s -> allocator type %s", selW, selH,
                selNv12 ? L"NV12" : L"RGB32",
                pInitType == m_pMediaTypeNv12 ? L"NV12720" : (pInitType == m_pMediaType640 ? L"RGB640" : L"RGB720"));
    // Log the delivered type once per session as well.
    m_lastDeliverW = 0;
    m_lastDeliverH = 0;
    m_lastDeliverNv12 = false;
    if (m_pAllocator != nullptr && pInitType != nullptr) {
        HRESULT hrInit = m_pAllocator->InitializeSampleAllocator(10, pInitType);
        if (FAILED(hrInit)) {
            VCamDiagLog(L"Stream.StartForSession InitializeSampleAllocator FAIL 0x%08X -> local fallback", hrInit);
            m_pAllocator->Release();
            m_pAllocator = nullptr;
        }
        else {
            VCamDiagLog(L"Stream.StartForSession InitializeSampleAllocator OK");
        }
    }
    m_workerStop = false;
    m_worker = std::thread(&CMediaStream::WorkerMain, this);

    PROPVARIANT vt;
    PropVariantInit(&vt);
    vt.vt = VT_I8;
    vt.hVal.QuadPart = MFGetSystemTime();
    HRESULT hr = QueueEvent(MEStreamStarted, GUID_NULL, S_OK, &vt);
    PropVariantClear(&vt);
    return hr;
}

HRESULT CMediaStream::StopForSession()
{
    VCamDiagLog(L"Stream.StopForSession");
    if (m_shutdown) return MF_E_SHUTDOWN;
    if (m_state == MF_STREAM_STATE_STOPPED) return S_OK;

    m_state = MF_STREAM_STATE_STOPPED;

    // Stop the worker thread
    {
        std::lock_guard<std::mutex> lk(m_tokenMutex);
        m_workerStop = true;
    }
    m_tokenCv.notify_all();
    if (m_worker.joinable() && std::this_thread::get_id() != m_worker.get_id()) {
        m_worker.join();
    }

    // Clear pending tokens
    {
        std::lock_guard<std::mutex> lk(m_tokenMutex);
        while (!m_tokens.empty()) {
            m_tokens.front()->Release();
            m_tokens.pop_front();
        }
    }

    if (m_pAllocator != nullptr) {
        HRESULT hrUninit = m_pAllocator->UninitializeSampleAllocator();
        VCamDiagLog(L"Stream.StopForSession UninitializeSampleAllocator -> 0x%08X", hrUninit);
    }

    PROPVARIANT vt;
    PropVariantInit(&vt);
    HRESULT hr = QueueEvent(MEStreamStopped, GUID_NULL, S_OK, &vt);
    PropVariantClear(&vt);
    return hr;
}

void CMediaStream::ShutDownInternal()
{
    if (m_shutdown) return;
    m_shutdown = true;

    // Stop the worker thread
    {
        std::lock_guard<std::mutex> lk(m_tokenMutex);
        m_workerStop = true;
    }
    m_tokenCv.notify_all();
    if (m_worker.joinable() && std::this_thread::get_id() != m_worker.get_id()) {
        m_worker.join();
    }

    // Clear pending tokens
    {
        std::lock_guard<std::mutex> lk(m_tokenMutex);
        while (!m_tokens.empty()) {
            m_tokens.front()->Release();
            m_tokens.pop_front();
        }
    }

    QueueEvent(MEError, GUID_NULL, MF_E_SHUTDOWN, nullptr);

    // Resource cleanup (event queue, descriptor, types, attributes, m_cs) is
    // deliberately deferred to ~CMediaStream: a client thread may already have
    // passed the m_shutdown check (RequestSample/GetEvent TOCTOU) and be inside
    // DeliverNextSample on m_cs — releasing here would be a UAF. The object
    // lives until the last COM reference is dropped, which orders cleanup
    // after every in-flight call.
}

void CMediaStream::WorkerMain()
{
    for (;;) {
        IUnknown* pToken = nullptr;
        {
            std::unique_lock<std::mutex> lk(m_tokenMutex);
            m_tokenCv.wait(lk, [&] { return m_workerStop || !m_tokens.empty(); });
            if (m_tokens.empty()) {
                if (m_workerStop) return;
                continue;
            }
            pToken = m_tokens.front();
            m_tokens.pop_front();
        }
        if (m_workerStop) { if (pToken) pToken->Release(); continue; }
        DeliverNextSample(pToken);
        if (pToken) pToken->Release();
    }
}

// Вписывает src в dst с сохранением пропорций (letterbox), центрирует и
// заливает поля чёрным. Без этого 16:9 (1280x720) сжимался в 4:3 (640x480)
// неравномерно (x*0.5, y*0.667) и картинка вытягивалась по вертикали в 1.33x.
static void DownsampleRgb32(const BYTE* pSrc, BYTE* pDst, UINT32 srcW, UINT32 srcH, UINT32 dstW, UINT32 dstH, UINT32 srcStride)
{
    UINT32 outW = dstW, outH = dstH;
    if ((UINT64)srcW * dstH > (UINT64)dstW * srcH) {
        outH = (UINT32)((UINT64)srcH * dstW / srcW); // вписываем по ширине
        if (outH == 0) outH = 1;
    } else {
        outW = (UINT32)((UINT64)srcW * dstH / srcH); // вписываем по высоте
        if (outW == 0) outW = 1;
    }
    const UINT32 offX = (dstW - outW) / 2;
    const UINT32 offY = (dstH - outH) / 2;

    memset(pDst, 0, (SIZE_T)dstW * dstH * 4);
    for (UINT32 y = 0; y < outH; ++y) {
        UINT32 srcY = (UINT32)((UINT64)y * srcH / outH);
        const BYTE* pSrcRow = pSrc + (SIZE_T)srcY * srcStride;
        BYTE* pDstRow = pDst + (SIZE_T)(y + offY) * (dstW * 4) + (SIZE_T)offX * 4;
        for (UINT32 x = 0; x < outW; ++x) {
            UINT32 srcX = (UINT32)((UINT64)x * srcW / outW);
            const BYTE* pSrcPixel = pSrcRow + (SIZE_T)srcX * 4;
            BYTE* pDstPixel = pDstRow + (SIZE_T)x * 4;
            pDstPixel[0] = pSrcPixel[0];
            pDstPixel[1] = pSrcPixel[1];
            pDstPixel[2] = pSrcPixel[2];
            pDstPixel[3] = pSrcPixel[3];
        }
    }
}

// BT.601 limited-range RGB -> Y + interleaved UV (2x2 subsampled).
// Shared-memory frames are BGRX in memory (ProducerApi.h, StaticImageSource,
// VCamPreview): byte0 = B, byte1 = G, byte2 = R (see CaptureTest "B at +0").
static void ConvertRgb32ToNv12(const BYTE* pSrc, BYTE* pDst, UINT32 width, UINT32 height, UINT32 srcStride)
{
    BYTE* pY = pDst;
    BYTE* pUV = pDst + (SIZE_T)width * height;

    for (UINT32 y = 0; y < height; ++y) {
        const BYTE* pRow = pSrc + (SIZE_T)y * srcStride;
        BYTE* pYRow = pY + (SIZE_T)y * width;
        for (UINT32 x = 0; x < width; ++x) {
            const BYTE* p = pRow + (SIZE_T)x * 4;
            const int b = p[0], g = p[1], r = p[2];
            pYRow[x] = (BYTE)(16 + ((66 * r + 129 * g + 25 * b + 128) >> 8));
            if ((x & 1u) == 0) {
                int rSum = r, gSum = g, bSum = b;
                int n = 1;
                if (x + 1 < width) {
                    const BYTE* p2 = p + 4;
                    rSum += p2[2]; gSum += p2[1]; bSum += p2[0];
                    ++n;
                }
                if (y + 1 < height) {
                    const BYTE* p3 = pSrc + (SIZE_T)(y + 1) * srcStride + (SIZE_T)x * 4;
                    rSum += p3[2]; gSum += p3[1]; bSum += p3[0];
                    ++n;
                    if (x + 1 < width) {
                        const BYTE* p4 = p3 + 4;
                        rSum += p4[2]; gSum += p4[1]; bSum += p4[0];
                        ++n;
                    }
                }
                const int rAvg = rSum / n, gAvg = gSum / n, bAvg = bSum / n;
                const SIZE_T uvIdx = ((SIZE_T)(y >> 1) * (width >> 1) + (x >> 1)) * 2;
                pUV[uvIdx] = (BYTE)(128 + ((-38 * rAvg - 74 * gAvg + 112 * bAvg + 128) >> 8));
                pUV[uvIdx + 1] = (BYTE)(128 + ((112 * rAvg - 94 * gAvg - 18 * bAvg + 128) >> 8));
            }
        }
    }
}

static void WriteFrameData(const BYTE* pSrcFrame, BYTE* pDstBits, UINT32 w, UINT32 h, bool useNv12)
{
    const UINT32 rgbBytes = w * h * 4;
    if (w == 640 && h == 480) {
        static thread_local BYTE rgb640[640 * 480 * 4];
        DownsampleRgb32(pSrcFrame, rgb640, 1280, 720, 640, 480, vcam::VCamStride);
        if (useNv12) {
            ConvertRgb32ToNv12(rgb640, pDstBits, 640, 480, 640 * 4);
        } else {
            memcpy(pDstBits, rgb640, rgbBytes);
        }
    } else {
        if (useNv12) {
            ConvertRgb32ToNv12(pSrcFrame, pDstBits, 1280, 720, vcam::VCamStride);
        } else {
            memcpy(pDstBits, pSrcFrame, rgbBytes);
        }
    }
}

void CMediaStream::ResolveNegotiatedType(UINT32* pW, UINT32* pH, bool* pNv12) const
{
    UINT32 w = m_selectedWidth;
    UINT32 h = m_selectedHeight;
    bool nv12 = m_selectedNv12;

    if (m_pStreamDescriptor != nullptr) {
        IMFMediaTypeHandler* pHandler = nullptr;
        if (SUCCEEDED(m_pStreamDescriptor->GetMediaTypeHandler(&pHandler)) && pHandler != nullptr) {
            IMFMediaType* pCur = nullptr;
            if (SUCCEEDED(pHandler->GetCurrentMediaType(&pCur)) && pCur != nullptr) {
                GUID sub = GUID_NULL;
                UINT32 hw = 0, hh = 0;
                if (SUCCEEDED(pCur->GetGUID(MF_MT_SUBTYPE, &sub)) &&
                    SUCCEEDED(MFGetAttributeSize(pCur, MF_MT_FRAME_SIZE, &hw, &hh)) &&
                    hw != 0 && hh != 0) {
                    if (sub == MFVideoFormat_NV12 || sub == MFVideoFormat_RGB32) {
                        w = hw;
                        h = hh;
                        nv12 = (sub == MFVideoFormat_NV12);
                    }
                }
                pCur->Release();
            }
            pHandler->Release();
        }
    }

    // Defensive: never deliver a size we cannot produce.
    if (!((w == vcam::VCamWidth && h == vcam::VCamHeight) || (w == 640 && h == 480))) {
        w = m_selectedWidth;
        h = m_selectedHeight;
        nv12 = m_selectedNv12;
    }

    if (pW != nullptr) *pW = w;
    if (pH != nullptr) *pH = h;
    if (pNv12 != nullptr) *pNv12 = nv12;
}

HRESULT CMediaStream::DeliverNextSample(IUnknown* pToken)
{
    VCamDiagLog(L"Stream.DeliverNextSample");
    HRESULT hr;
    IMFMediaBuffer* pBuffer = nullptr;
    IMFSample* pSample = nullptr;
    BYTE* pBits = nullptr;
    bool shared = false;

    // Format negotiation: the frameserver proxy may set the type via the
    // stream's SetMediaType OR via the SD handler's current type; honor both,
    // with the handler as the authoritative source (consumers read it).
    UINT32 w = 0, h = 0;
    bool useNv12 = false;
    ResolveNegotiatedType(&w, &h, &useNv12);
    const UINT32 rgbBytes = w * h * 4;
    const UINT32 nv12Bytes = w * h * 3 / 2;
    const bool typeChanged = (w != m_lastDeliverW) || (h != m_lastDeliverH) || (useNv12 != m_lastDeliverNv12);
    if (typeChanged) {
        VCamDiagLog(L"Stream.DeliverNextSample deliver type %ux%u %s (selected=%ux%u %s)", w, h,
                    useNv12 ? L"NV12" : L"RGB32", m_selectedWidth, m_selectedHeight,
                    m_selectedNv12 ? L"NV12" : L"RGB32");
        m_lastDeliverW = w;
        m_lastDeliverH = h;
        m_lastDeliverNv12 = useNv12;
    }

    EnterCriticalSection(&m_cs);
    if (m_pNv12Scratch == nullptr) m_pNv12Scratch = new (std::nothrow) BYTE[vcam::VCamFrameSize];
    LeaveCriticalSection(&m_cs);

    if (m_pAllocator != nullptr) {
        VCamDiagLog(L"Stream.DeliverNextSample before AllocateSample");
        hr = m_pAllocator->AllocateSample(&pSample);
        VCamDiagLog(L"Stream.DeliverNextSample AllocateSample hr=0x%08X sample=%p", (unsigned)hr, (const void*)pSample);
        if (SUCCEEDED(hr)) shared = true;
    }

    if (!shared) {
        // Fallback: local buffer (only readable in-session).
        hr = MFCreateMemoryBuffer((DWORD)(useNv12 ? nv12Bytes : rgbBytes), &pBuffer);
        if (FAILED(hr)) return hr;
        hr = pBuffer->Lock(&pBits, nullptr, nullptr);
        if (SUCCEEDED(hr)) {
            if (m_pNv12Scratch != nullptr) {
                SharedMemoryFrameSource::Instance().AcquireFrame(m_pNv12Scratch, vcam::VCamReadyTimeoutMs);
                WriteFrameData(m_pNv12Scratch, pBits, w, h, useNv12);
            } else {
                RtlZeroMemory(pBits, (DWORD)(useNv12 ? nv12Bytes : rgbBytes));
            }
            pBuffer->Unlock();
        }
        hr = MFCreateSample(&pSample);
        if (FAILED(hr)) { pBuffer->Release(); return hr; }
        HRESULT hrCur = pBuffer->SetCurrentLength(useNv12 ? nv12Bytes : rgbBytes);
        if (FAILED(hrCur)) { pBuffer->Release(); return hr; }
        hr = pSample->AddBuffer(pBuffer);
        pBuffer->Release();
        if (FAILED(hr)) { pSample->Release(); return hr; }
        VCamDiagLog(L"Stream.DeliverNextSample local buffer (no shared allocator)");
    }
    else {
        // Shared sample already carries a correctly-sized buffer.
        VCamDiagLog(L"Stream.DeliverNextSample before GetBufferByIndex");
        hr = pSample->GetBufferByIndex(0, &pBuffer);
        VCamDiagLog(L"Stream.DeliverNextSample GetBufferByIndex hr=0x%08X buf=%p", (unsigned)hr, (const void*)pBuffer);
        if (SUCCEEDED(hr)) {
            // Lock signature: (buffer, pcbMaxLength, pcbCurrentLength).
            DWORD maxLen = 0, curLen = 0;
            VCamDiagLog(L"Stream.DeliverNextSample before Lock");
            hr = pBuffer->Lock(&pBits, &maxLen, &curLen);
            VCamDiagEvent(L"Stream.DeliverNextSample Lock hr=0x%08X bits=%p maxLen=%u curLen=%u", (unsigned)hr, (const void*)pBits, maxLen, curLen);
        if (SUCCEEDED(hr)) {
            const DWORD needed = useNv12 ? nv12Bytes : rgbBytes;
            VCamDiagLog(L"Stream.DeliverNextSample before AcquireFrame");
            if (m_pNv12Scratch != nullptr && maxLen >= needed) {
                SharedMemoryFrameSource::Instance().AcquireFrame(m_pNv12Scratch, vcam::VCamReadyTimeoutMs);
                WriteFrameData(m_pNv12Scratch, pBits, w, h, useNv12);
                hr = pBuffer->SetCurrentLength(needed);
            }
            else {
                // Buffer too small for the negotiated frame: deliver an empty,
                // zeroed buffer rather than a partially valid one.
                RtlZeroMemory(pBits, maxLen);
                hr = pBuffer->SetCurrentLength(0);
                VCamDiagEvent(L"Stream.DeliverNextSample buffer too small maxLen=%u needed=%u", maxLen, needed);
            }
            VCamDiagLog(L"Stream.DeliverNextSample AcquireFrame done, before Unlock");
            pBuffer->Unlock();
            VCamDiagLog(L"Stream.DeliverNextSample Unlock done");
        }
        pBuffer->Release();
        }
        VCamDiagLog(L"Stream.DeliverNextSample shared sample allocated");
    }

    if (pSample == nullptr) return E_FAIL;

    LONGLONG time = MFGetSystemTime();
    hr = pSample->SetSampleTime(time);
    if (SUCCEEDED(hr)) hr = pSample->SetSampleDuration(vcam::VCamFrameInterval100ns);
    if (SUCCEEDED(hr) && pToken) hr = pSample->SetUnknown(MFSampleExtension_Token, pToken);
    if (FAILED(hr)) { pSample->Release(); return hr; }

    PROPVARIANT vtSample;
    PropVariantInit(&vtSample);
    vtSample.vt = VT_UNKNOWN;
    vtSample.punkVal = pSample;
    hr = QueueEvent(MEMediaSample, GUID_NULL, S_OK, &vtSample);
    PropVariantClear(&vtSample);
    return hr;
}
