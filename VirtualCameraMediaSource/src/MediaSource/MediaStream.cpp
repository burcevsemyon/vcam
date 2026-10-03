#define INITGUID
#include <windows.h>
#include <cguid.h>
#include "MediaStream.h"
#include "MediaSource.h"
#include "SharedMemoryContract.h"
#include "SharedMemoryFrameSource.h"
#include "FrameCopy.h"
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
    m_pEventQueue = nullptr;
    m_pTypeHandler = nullptr;
    m_pStreamDescriptor = nullptr;
    m_pMediaType = nullptr;
    m_pMediaTypeNv12 = nullptr;
    m_pMediaType640 = nullptr;
    m_pMediaTypeNative = nullptr;
    m_nativeW = m_nativeH = 0;
    m_pV2Staging.reset();
    m_cbV2Staging = 0;
    m_pNv12Scratch.reset(); // порядок освобождений как с raw delete[] (до attrs/allocator)
    m_pStreamAttrsProxy = nullptr;
    m_pStreamAttributes = nullptr;
    m_pAllocator = nullptr;
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
        IMFAttributes* pAttrs = (m_pStreamAttrsProxy.p != nullptr)
            ? static_cast<IMFAttributes*>(m_pStreamAttrsProxy.p) : m_pStreamAttributes.p;
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
    if (SUCCEEDED(hr)) {
        m_pAllocator.Attach(pShared); // takes ownership; releases the previous allocator
        VCamDiagLog(L"Stream.SetAllocator shared=OK");
    }
    else {
        m_pAllocator = nullptr;
        VCamDiagLog(L"Stream.SetAllocator shared=no (0x%08X) -> local fallback", hr);
    }
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
    return m_pStreamDescriptor.CopyTo(ppDescriptor); // assigns + AddRef (COM contract)
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
    // Лесенка: 1280x720 (RGB32/NV12) + 640x480 как раньше (включая исторически
    // принимаемый NV12-640 — приёмку не меняем); плюс натив v2 (RGB32 w×h из
    // заголовка, != 720p). NV12-натив НЕ принимаем.
    const bool isStandard = ((w == 1280 && h == 720) || (w == 640 && h == 480));
    if (!isStandard) {
        if (subtype != MFVideoFormat_RGB32) return MF_E_INVALIDMEDIATYPE;
        if (m_nativeW == 0 || m_nativeH == 0 || w != m_nativeW || h != m_nativeH)
            return MF_E_INVALIDMEDIATYPE;
    }

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
        ATL::CComPtr<IMFMediaTypeHandler> pHandler;
        if (SUCCEEDED(m_pStreamDescriptor->GetMediaTypeHandler(&pHandler)) && pHandler != nullptr) {
            HRESULT hrSync = pHandler->SetCurrentMediaType(pMediaType);
            VCamDiagLog(L"Stream.SetMediaType handler SetCurrentMediaType hr=0x%08X", (unsigned)hrSync);
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

    // Натив v2: RGB32 w×h@30 из живого заголовка v2 (read-only проба). Нет v2
    // или натив == 720p — дубликат НЕ рекламируем, лесенка = старые 3 типа.
    // NV12-натив НЕ добавляем.
    {
        UINT32 v2w = 0, v2h = 0, v2stride = 0, v2fs = 0;
        if (m_v2.Probe(&v2w, &v2h, &v2stride, &v2fs) &&
            vcam_v2::ShouldAdvertiseNative(v2w, v2h)) {
            ATL::CComPtr<IMFMediaType> pNative;
            hr = MFCreateMediaType(&pNative);
            if (SUCCEEDED(hr)) hr = pNative->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
            if (SUCCEEDED(hr)) hr = pNative->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
            if (SUCCEEDED(hr)) hr = MFSetAttributeSize(pNative, MF_MT_FRAME_SIZE, v2w, v2h);
            if (SUCCEEDED(hr)) hr = MFSetAttributeRatio(pNative, MF_MT_FRAME_RATE, 30, 1);
            if (SUCCEEDED(hr)) hr = pNative->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
            if (SUCCEEDED(hr)) hr = pNative->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
            if (SUCCEEDED(hr)) hr = pNative->SetUINT32(MF_MT_FIXED_SIZE_SAMPLES, TRUE);
            if (SUCCEEDED(hr)) hr = pNative->SetUINT32(MF_MT_DEFAULT_STRIDE, v2w * 4);
            if (SUCCEEDED(hr)) hr = pNative->SetUINT32(MF_MT_SAMPLE_SIZE, v2fs);
            if (SUCCEEDED(hr)) {
                const UINT64 bps = (UINT64)v2fs * 8 * 30;
                hr = pNative->SetUINT32(MF_MT_AVG_BITRATE,
                                        bps > 0xFFFFFFFFull ? 0xFFFFFFFFu : (UINT32)bps);
            }
            if (SUCCEEDED(hr)) hr = MFSetAttributeRatio(pNative, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
            if (SUCCEEDED(hr)) {
                m_pMediaTypeNative = pNative;
                m_nativeW = v2w;
                m_nativeH = v2h;
                VCamDiagLog(L"Stream.FinalConstruct native v2 %ux%u advertised", v2w, v2h);
            }
            else {
                VCamDiagLog(L"Stream.FinalConstruct native v2 %ux%u NOT advertised hr=0x%08X", v2w, v2h, (unsigned)hr);
                m_pMediaTypeNative = nullptr;
                hr = S_OK; // натив — best effort, лесенка остаётся из 3 типов
            }
        }
    }

    IMFMediaType* types[4] = { m_pMediaType, m_pMediaTypeNv12, m_pMediaType640, m_pMediaTypeNative };
    const DWORD typeCount = (m_pMediaTypeNative != nullptr) ? 4 : 3;
    hr = MFCreateStreamDescriptor(0, typeCount, types, &m_pStreamDescriptor);
    if (FAILED(hr)) return hr;

    // The frameserver's Start validation calls GetCurrentMediaType on the
    // handler; without an explicit current type it fails with
    // MF_E_INVALIDTYPE (0xC00D36BD).
    ATL::CComPtr<IMFMediaTypeHandler> pHandler;
    hr = m_pStreamDescriptor->GetMediaTypeHandler(&pHandler);
    if (FAILED(hr)) return hr;
    hr = pHandler->SetCurrentMediaType(m_pMediaType);
    if (FAILED(hr)) return hr;
    m_pTypeHandler = pHandler; // fixed for the descriptor's lifetime: skips the per-frame GetMediaTypeHandler QI
    pHandler = nullptr;

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
    m_pStreamAttrsProxy.Attach(new (std::nothrow) CAttrLogProxy(m_pStreamAttributes, L"Stream"));
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
    SharedMemoryFrameSource::Instance().TouchReader(); // heartbeat потребителя с 1-го момента сессии
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
    else if (!selNv12 && m_pMediaTypeNative != nullptr && selW == m_nativeW && selH == m_nativeH) {
        pInitType = m_pMediaTypeNative;
    }
    else {
        pInitType = m_pMediaType;
    }
    VCamDiagEvent(L"Stream.StartForSession negotiated %ux%u %s -> allocator type %s", selW, selH,
                selNv12 ? L"NV12" : L"RGB32",
                pInitType == m_pMediaTypeNv12 ? L"NV12720" : (pInitType == m_pMediaType640 ? L"RGB640" : (pInitType == m_pMediaTypeNative ? L"RGBnative" : L"RGB720")));
    // Log the delivered type once per session as well.
    m_lastDeliverW = 0;
    m_lastDeliverH = 0;
    m_lastDeliverNv12 = false;
    if (m_pAllocator != nullptr && pInitType != nullptr) {
        HRESULT hrInit = m_pAllocator->InitializeSampleAllocator(10, pInitType);
        if (FAILED(hrInit)) {
            VCamDiagLog(L"Stream.StartForSession InitializeSampleAllocator FAIL 0x%08X -> local fallback", hrInit);
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
        ATL::CComPtr<IUnknown> pToken;
        {
            std::unique_lock<std::mutex> lk(m_tokenMutex);
            m_tokenCv.wait(lk, [&] { return m_workerStop || !m_tokens.empty(); });
            if (m_tokens.empty()) {
                if (m_workerStop) return;
                continue;
            }
            pToken = std::move(m_tokens.front());
            m_tokens.pop_front();
        }
        if (m_workerStop) { continue; }
        DeliverNextSample(pToken);
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

    if (m_pTypeHandler != nullptr) {
        // Handler закэширован в FinalConstruct; текущий тип читается живьём
        // на каждый кадр — внешние писатели (frameserver-прокси, прямой
        // SetCurrentMediaType у handler) не уведомляют нас, поэтому
        // результат resolve кэшировать нельзя (см. MediaStream.h).
        ATL::CComPtr<IMFMediaType> pCur;
        if (SUCCEEDED(m_pTypeHandler->GetCurrentMediaType(&pCur)) && pCur != nullptr) {
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
        }
    }

    // Defensive: never deliver a size we cannot produce. Стандарт — 720p
    // (RGB32/NV12) и 640x480; плюс живой натив v2 (RGB32 == текущим диметрам
    // заголовка — покрывает и рекламируемый, и сменившийся hot-switch'ем).
    // NV12-натив не производим.
    if (!((w == vcam::VCamWidth && h == vcam::VCamHeight) || (w == 640 && h == 480))) {
        bool liveNative = false;
        if (!nv12) {
            UINT32 vw = 0, vh = 0, vs = 0, vfs = 0;
            if (m_v2.Probe(&vw, &vh, &vs, &vfs) && vw == w && vh == h)
                liveNative = true;
        }
        if (!liveNative) {
            w = m_selectedWidth;
            h = m_selectedHeight;
            nv12 = m_selectedNv12;
        }
    }

    if (pW != nullptr) *pW = w;
    if (pH != nullptr) *pH = h;
    if (pNv12 != nullptr) *pNv12 = nv12;
}

bool CMediaStream::AcquireV2Frame(UINT32* pW, UINT32* pH, UINT32* pStride)
{
    // Стейджинг — фиксированный max-размер один раз (как m_pNv12Scratch):
    // указатель после первой аллокации не меняется, поэтому конкурентные
    // доставщики не получают UAF (realloc по диметрам запрещён).
    EnterCriticalSection(&m_cs);
    if (m_pV2Staging == nullptr) {
        m_pV2Staging.reset(new (std::nothrow) BYTE[vcam::VCamV2MaxFrameSize]);
        m_cbV2Staging = (m_pV2Staging != nullptr) ? (SIZE_T)vcam::VCamV2MaxFrameSize : 0;
    }
    BYTE* pStaging = m_pV2Staging.get();
    const SIZE_T cbStaging = m_cbV2Staging;
    LeaveCriticalSection(&m_cs);
    if (pStaging == nullptr) return false;
    UINT32 aw = 0, ah = 0, as = 0;
    if (!m_v2.Acquire(pStaging, cbStaging, &aw, &ah, &as, vcam::VCamReadyTimeoutMs))
        return false;
    if (pW) *pW = aw;
    if (pH) *pH = ah;
    if (pStride) *pStride = as;
    return true;
}

bool CMediaStream::DeliverFromV2(BYTE* pBits, UINT32 w, UINT32 h, bool useNv12,
                                 UINT32 vw, UINT32 vh, UINT32 vstride)
{
    const BYTE* pSrc = m_pV2Staging.get();
    if (pBits == nullptr || pSrc == nullptr) return false;
    if (!useNv12) {
        if (w == vw && h == vh) {
            // Натив 1:1 (memcpy в sample, alpha — как старые пути).
            vcam::CopyFrameRowwise(pBits, (SIZE_T)w * 4, pSrc, vstride, w, h, 4);
            return true;
        }
        // 720p/640p/протухший-натив: letterbox-fit из живого v2
        // (16:9 fill, иное — letterbox). Безопасно для любого w/h: пишется
        // ровно w*h*4 под размер буфера цели.
        vcam_v2::DownscaleRgb32Letterbox(pSrc, vstride, vw, vh,
                                         pBits, (SIZE_T)w * 4, w, h);
        return true;
    }
    if (w == vcam::VCamWidth && h == vcam::VCamHeight && m_pNv12Scratch != nullptr) {
        // NV12-720: даунскейл v2 в скретч + та же конверсия, что раньше.
        vcam_v2::DownscaleRgb32Letterbox(pSrc, vstride, vw, vh,
                                         m_pNv12Scratch.get(), vcam::VCamStride,
                                         vcam::VCamWidth, vcam::VCamHeight);
        ConvertRgb32ToNv12(m_pNv12Scratch.get(), pBits,
                           vcam::VCamWidth, vcam::VCamHeight, vcam::VCamStride);
        return true;
    }
    return false; // NV12 не-720 — старый v1-путь (NV12-640 бит-в-бит)
}

HRESULT CMediaStream::DeliverNextSample(IUnknown* pToken)
{
    VCamDiagLog(L"Stream.DeliverNextSample");
    HRESULT hr;
    ATL::CComPtr<IMFMediaBuffer> pBuffer;
    ATL::CComPtr<IMFSample> pSample;
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
    if (m_pNv12Scratch == nullptr) m_pNv12Scratch.reset(new (std::nothrow) BYTE[vcam::VCamFrameSize]);
    LeaveCriticalSection(&m_cs);

    // v2 первым: свежий валидный натив-кадр. Нет/мусор/таймаут — СТАРЫЙ
    // v1-путь ниже бит-в-бит (регресс D/E-логики).
    UINT32 v2w = 0, v2h = 0, v2stride = 0;
    const bool haveV2 = AcquireV2Frame(&v2w, &v2h, &v2stride);

    if (m_pAllocator != nullptr) {
        VCamDiagLog(L"Stream.DeliverNextSample before AllocateSample");
        hr = m_pAllocator->AllocateSample(&pSample);
        VCamDiagLog(L"Stream.DeliverNextSample AllocateSample hr=0x%08X sample=%p", (unsigned)hr, (const void*)pSample.p);
        if (SUCCEEDED(hr)) shared = true;
    }

    if (!shared) {
        // Fallback: local buffer (only readable in-session).
        hr = MFCreateMemoryBuffer((DWORD)(useNv12 ? nv12Bytes : rgbBytes), &pBuffer);
        if (FAILED(hr)) return hr;
        hr = pBuffer->Lock(&pBits, nullptr, nullptr);
        if (SUCCEEDED(hr)) {
            if (haveV2 && DeliverFromV2(pBits, w, h, useNv12, v2w, v2h, v2stride)) {
                // Кадр из v2 (натив 1:1 / letterbox-fit / NV12-720).
            }
            else if (!haveV2 && m_pNv12Scratch != nullptr &&
                     ((w == vcam::VCamWidth && h == vcam::VCamHeight) || (w == 640 && h == 480))) {
                if (!useNv12 && w == vcam::VCamWidth && h == vcam::VCamHeight) {
                    // RGB32 1280x720: буфер создан ровно под rgbBytes ==
                    // VCamFrameSize — пишем кадр сразу в sample (без копии
                    // scratch→sample); NV12/640-ветки идут через scratch.
                    SharedMemoryFrameSource::Instance().AcquireFrame(pBits, vcam::VCamReadyTimeoutMs);
                } else {
                    SharedMemoryFrameSource::Instance().AcquireFrame(m_pNv12Scratch.get(), vcam::VCamReadyTimeoutMs);
                    WriteFrameData(m_pNv12Scratch.get(), pBits, w, h, useNv12);
                }
            } else if (!haveV2 && m_pNv12Scratch != nullptr) {
                // Протухший натив без живого v2 из v1 не произвести (там
                // только 720p): нулевой кадр вместо переполнения WriteFrameData.
                VCamDiagEvent(L"Stream.DeliverNextSample no v2 for %ux%u -> zero fill", w, h);
                RtlZeroMemory(pBits, (DWORD)(useNv12 ? nv12Bytes : rgbBytes));
            } else if (haveV2 && m_pNv12Scratch != nullptr) {
                // v2 жив, но цель — NV12 не-720: старый путь (NV12-640 бит-в-бит).
                SharedMemoryFrameSource::Instance().AcquireFrame(m_pNv12Scratch.get(), vcam::VCamReadyTimeoutMs);
                WriteFrameData(m_pNv12Scratch.get(), pBits, w, h, useNv12);
            } else {
                RtlZeroMemory(pBits, (DWORD)(useNv12 ? nv12Bytes : rgbBytes));
            }
            pBuffer->Unlock();
        }
        hr = MFCreateSample(&pSample);
        if (FAILED(hr)) { pBuffer = nullptr; return hr; }
        HRESULT hrCur = pBuffer->SetCurrentLength(useNv12 ? nv12Bytes : rgbBytes);
        if (FAILED(hrCur)) { pBuffer = nullptr; return hr; }
        hr = pSample->AddBuffer(pBuffer);
        pBuffer = nullptr;
        if (FAILED(hr)) { pSample = nullptr; return hr; }
        VCamDiagLog(L"Stream.DeliverNextSample local buffer (no shared allocator)");
    }
    else {
        // Shared sample already carries a correctly-sized buffer.
        VCamDiagLog(L"Stream.DeliverNextSample before GetBufferByIndex");
        hr = pSample->GetBufferByIndex(0, &pBuffer);
        VCamDiagLog(L"Stream.DeliverNextSample GetBufferByIndex hr=0x%08X buf=%p", (unsigned)hr, (const void*)pBuffer.p);
        if (SUCCEEDED(hr)) {
            // Lock signature: (buffer, pcbMaxLength, pcbCurrentLength).
            DWORD maxLen = 0, curLen = 0;
            VCamDiagLog(L"Stream.DeliverNextSample before Lock");
            hr = pBuffer->Lock(&pBits, &maxLen, &curLen);
            VCamDiagEvent(L"Stream.DeliverNextSample Lock hr=0x%08X bits=%p maxLen=%u curLen=%u", (unsigned)hr, (const void*)pBits, maxLen, curLen);
        if (SUCCEEDED(hr)) {
            const DWORD needed = useNv12 ? nv12Bytes : rgbBytes;
            VCamDiagLog(L"Stream.DeliverNextSample before AcquireFrame");
            if (haveV2 && maxLen >= needed &&
                DeliverFromV2(pBits, w, h, useNv12, v2w, v2h, v2stride)) {
                // Кадр из v2 (натив 1:1 / letterbox-fit / NV12-720).
                hr = pBuffer->SetCurrentLength(needed);
            }
            else if (!haveV2 && m_pNv12Scratch != nullptr && maxLen >= needed &&
                     ((w == vcam::VCamWidth && h == vcam::VCamHeight) || (w == 640 && h == 480))) {
                if (!useNv12 && w == vcam::VCamWidth && h == vcam::VCamHeight) {
                    // RGB32 1280x720: maxLen >= needed == VCamFrameSize — прямая
                    // запись кадра в буфер sample (без копии scratch→sample);
                    // NV12/640-ветки остаются на scratch (их WriteFrameData).
                    SharedMemoryFrameSource::Instance().AcquireFrame(pBits, vcam::VCamReadyTimeoutMs);
                } else {
                    SharedMemoryFrameSource::Instance().AcquireFrame(m_pNv12Scratch.get(), vcam::VCamReadyTimeoutMs);
                    WriteFrameData(m_pNv12Scratch.get(), pBits, w, h, useNv12);
                }
                hr = pBuffer->SetCurrentLength(needed);
            }
            else if (!haveV2 && maxLen >= needed) {
                // Протухший натив (или нет скретча): нулевой кадр вместо
                // переполнения WriteFrameData — v1 знает только 720p/640p.
                VCamDiagEvent(L"Stream.DeliverNextSample no v2 for %ux%u -> empty sample", w, h);
                RtlZeroMemory(pBits, maxLen);
                hr = pBuffer->SetCurrentLength(0);
            }
            else if (haveV2 && m_pNv12Scratch != nullptr && maxLen >= needed) {
                // v2 жив, но цель — NV12 не-720: старый путь (NV12-640 бит-в-бит).
                SharedMemoryFrameSource::Instance().AcquireFrame(m_pNv12Scratch.get(), vcam::VCamReadyTimeoutMs);
                WriteFrameData(m_pNv12Scratch.get(), pBits, w, h, useNv12);
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
        pBuffer = nullptr;
        }
        VCamDiagLog(L"Stream.DeliverNextSample shared sample allocated");
    }

    if (pSample == nullptr) return E_FAIL;

    LONGLONG time = MFGetSystemTime();
    hr = pSample->SetSampleTime(time);
    if (SUCCEEDED(hr)) hr = pSample->SetSampleDuration(vcam::VCamFrameInterval100ns);
    if (SUCCEEDED(hr) && pToken) hr = pSample->SetUnknown(MFSampleExtension_Token, pToken);
    if (FAILED(hr)) { pSample = nullptr; return hr; }

    PROPVARIANT vtSample;
    PropVariantInit(&vtSample);
    vtSample.vt = VT_UNKNOWN;
    vtSample.punkVal = pSample.Detach(); // transfer to PROPVARIANT; PropVariantClear releases
    hr = QueueEvent(MEMediaSample, GUID_NULL, S_OK, &vtSample);
    PropVariantClear(&vtSample);
    return hr;
}
