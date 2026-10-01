#include <windows.h>
#include <cstdio>
#include <new>
#include "Activator.h"
#include "MediaSource.h"
#include <Mferror.h>

void VCamDiagLog(const wchar_t* fmt, ...);
void VCamDiagGuid(const GUID* g, wchar_t* out, size_t cch);

// The local SDK's hstring.h (winrt\) declares the HSTRING type but not the factory.
extern "C" HRESULT WINAPI WindowsCreateString(PCWSTR sourceString, UINT32 length, HSTRING* resultString);

// MF_VIRTUALCAMERA_PROVIDE_ASSOCIATED_CAMERA_SOURCES (mfvirtualcamera.h).
// The FrameServer reads this UINT32 on the IMFActivate object; this camera
// provides no associated camera sources, so the value is 0.
static const GUID kVcamProvideAssociatedCameraSources =
    { 0xf0273718, 0x4a4d, 0x4ac5, { 0xa1, 0x5d, 0x30, 0x5e, 0xb5, 0xe9, 0x06, 0x67 } };

CVCamActivator::CVCamActivator()
{
    VCamObjectInc();
}

CVCamActivator::~CVCamActivator()
{
    VCamDiagLog(L"Act.~dtor");
    if (m_source) m_source->Release();
    if (m_attrs) m_attrs->Release();
    VCamObjectDec();
}

HRESULT CVCamActivator::QueryInterface(REFIID riid, void** ppvObject)
{
    if (ppvObject == nullptr) return E_POINTER;
    HRESULT hr = E_NOINTERFACE;
    // IMFActivate derives from IMFAttributes; each interface gets its own
    // properly-adjusted subobject pointer.
    if (riid == IID_IUnknown ||
        riid == IID_IMFActivate ||
        riid == IID_IMFAttributes) {
        *ppvObject = static_cast<IMFActivate*>(this);
        AddRef();
        hr = S_OK;
    }
    else if (riid == IID_IKsControl) {
        *ppvObject = static_cast<IKsControl*>(this);
        AddRef();
        hr = S_OK;
    }
    else if (riid == IID_IInspectable ||
        riid == kIID_IInspectable_Canonical) {
        *ppvObject = static_cast<IInspectable*>(this);
        AddRef();
        hr = S_OK;
    }
    else {
        *ppvObject = nullptr;
    }
    wchar_t guidStr[64];
    VCamDiagGuid(&riid, guidStr, 64);
    VCamDiagLog(L"Act::QI %s -> 0x%08X", guidStr, hr);
    return hr;
}

STDAPI_(ULONG) CVCamActivator::AddRef()
{
    return InternalAddRef();
}

STDAPI_(ULONG) CVCamActivator::Release()
{
    ULONG ref = InternalRelease();
    if (ref == 0) delete this;
    return ref;
}

// IInspectable
STDMETHODIMP CVCamActivator::GetIids(ULONG* iidCount, IID** iids)
{
    if (iidCount == nullptr) return E_POINTER;
    static const IID s_iids[] = {
        __uuidof(IUnknown),
        __uuidof(IMFActivate),
        IID_IMFAttributes,
        IID_IKsControl,
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

STDMETHODIMP CVCamActivator::GetRuntimeClassName(HSTRING* className)
{
    if (className == nullptr) return E_POINTER;
    return WindowsCreateString(L"VirtualCameraMediaSource.Activator", 0, className);
}

STDMETHODIMP CVCamActivator::GetTrustLevel(TrustLevel* trustLevel)
{
    if (trustLevel == nullptr) return E_POINTER;
    *trustLevel = BaseTrust;
    return S_OK;
}

HRESULT CVCamActivator::ActivateObject(REFIID riid, void** ppv)
{
    if (ppv == nullptr) return E_POINTER;
    *ppv = nullptr;
    wchar_t guidStr[64];
    VCamDiagGuid(&riid, guidStr, 64);
    VCamDiagLog(L"Act.ActivateObject %s", guidStr);
    if (m_source == nullptr)
    {
        CMediaSource* pSource = new (std::nothrow) CMediaSource();
        if (pSource == nullptr) return E_OUTOFMEMORY;
        pSource->AddRef();
        HRESULT hr = pSource->FinalConstruct();
        if (SUCCEEDED(hr)) {
            // Parity with smourier/VCamSample: the source inherits the activator's
            // attributes (FRIENDLY_NAME etc.) in the store the frameserver reads.
            hr = pSource->CopyActivationAttributes(m_attrs);
        }
        if (SUCCEEDED(hr)) {
            // CopyAllItems replaced the (fresh) source store, so the source's own
            // attributes — including MF_DEVICEMFT_SENSORPROFILE_COLLECTION —
            // must be applied only now.
            hr = pSource->SetIntrinsicAttributes();
        }
        if (SUCCEEDED(hr)) {
            m_source = pSource;
        }
        else {
            pSource->Release();
            return hr;
        }
    }
    return m_source->QueryInterface(riid, ppv);
}

HRESULT CVCamActivator::ShutdownObject()
{
    VCamDiagLog(L"Act.ShutdownObject");
    if (m_source) {
        m_source->Shutdown();
        m_source->Release();
        m_source = nullptr;
    }
    return S_OK;
}

HRESULT CVCamActivator::DetachObject()
{
    VCamDiagLog(L"Act.DetachObject");
    if (m_source) {
        m_source->Release();
        m_source = nullptr;
    }
    return S_OK;
}

// IKsControl stubs (no KS properties exposed)
HRESULT CVCamActivator::KsProperty(PKSPROPERTY Property, ULONG PropertyLength, LPVOID PropertyData, ULONG DataLength, ULONG* BytesReturned)
{
    wchar_t guidStr[64];
    if (Property != nullptr) {
        VCamDiagGuid(&Property->Set, guidStr, 64);
        VCamDiagLog(L"Act.KsProperty set=%s id=%u -> ERROR_SET_NOT_FOUND", guidStr, Property->Id);
    }
    else VCamDiagLog(L"Act.KsProperty (null) -> ERROR_SET_NOT_FOUND");
    return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}

HRESULT CVCamActivator::KsMethod(PKSMETHOD Method, ULONG MethodLength, LPVOID MethodData, ULONG DataLength, ULONG* BytesReturned)
{
    wchar_t guidStr[64];
    if (Method != nullptr) {
        VCamDiagGuid(&Method->Set, guidStr, 64);
        VCamDiagLog(L"Act.KsMethod set=%s id=%u -> ERROR_SET_NOT_FOUND", guidStr, Method->Id);
    }
    else VCamDiagLog(L"Act.KsMethod (null) -> ERROR_SET_NOT_FOUND");
    return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}

HRESULT CVCamActivator::KsEvent(PKSEVENT Event, ULONG EventLength, LPVOID EventData, ULONG DataLength, ULONG* BytesReturned)
{
    wchar_t guidStr[64];
    if (Event != nullptr) {
        VCamDiagGuid(&Event->Set, guidStr, 64);
        VCamDiagLog(L"Act.KsEvent set=%s id=%u -> ERROR_SET_NOT_FOUND", guidStr, Event->Id);
    }
    else VCamDiagLog(L"Act.KsEvent (null) -> ERROR_SET_NOT_FOUND");
    return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}

// IMFAttributes - delegates to the activation attribute store (logged for diagnosis)
#define ACT_ATTR_FWD(name, args, call) \
    HRESULT CVCamActivator::name args \
    { \
        HRESULT _hr = m_attrs ? m_attrs->call : E_NOTIMPL; \
        VCamDiagLog(L"Act.%s -> 0x%08X", L"" #name, _hr); \
        return _hr; \
    }
#define ACT_ATTR_FWD_K(name, args, call) \
    HRESULT CVCamActivator::name args \
    { \
        HRESULT _hr = m_attrs ? m_attrs->call : E_NOTIMPL; \
        wchar_t _keyStr[64]; \
        VCamDiagGuid(&guidKey, _keyStr, 64); \
        VCamDiagLog(L"Act.%s %s -> 0x%08X", L"" #name, _keyStr, _hr); \
        return _hr; \
    }
ACT_ATTR_FWD_K(GetItem, (REFGUID guidKey, PROPVARIANT* pValue), GetItem(guidKey, pValue))
ACT_ATTR_FWD_K(GetItemType, (REFGUID guidKey, MF_ATTRIBUTE_TYPE* pType), GetItemType(guidKey, pType))
ACT_ATTR_FWD_K(CompareItem, (REFGUID guidKey, REFPROPVARIANT Value, BOOL* pbResult), CompareItem(guidKey, Value, pbResult))
ACT_ATTR_FWD(Compare, (IMFAttributes* pTheirs, MF_ATTRIBUTES_MATCH_TYPE MatchType, BOOL* pbResult), Compare(pTheirs, MatchType, pbResult))
ACT_ATTR_FWD_K(GetUINT32, (REFGUID guidKey, UINT32* punValue), GetUINT32(guidKey, punValue))
ACT_ATTR_FWD_K(GetUINT64, (REFGUID guidKey, UINT64* punValue), GetUINT64(guidKey, punValue))
ACT_ATTR_FWD_K(GetDouble, (REFGUID guidKey, double* pfValue), GetDouble(guidKey, pfValue))
ACT_ATTR_FWD_K(GetGUID, (REFGUID guidKey, GUID* pguidValue), GetGUID(guidKey, pguidValue))
ACT_ATTR_FWD_K(GetStringLength, (REFGUID guidKey, UINT32* pcchLength), GetStringLength(guidKey, pcchLength))
ACT_ATTR_FWD_K(GetString, (REFGUID guidKey, LPWSTR pwszValue, UINT32 cchBufSize, UINT32* pcchLength), GetString(guidKey, pwszValue, cchBufSize, pcchLength))
ACT_ATTR_FWD_K(GetAllocatedString, (REFGUID guidKey, LPWSTR* ppwszValue, UINT32* pcchLength), GetAllocatedString(guidKey, ppwszValue, pcchLength))
ACT_ATTR_FWD_K(GetBlobSize, (REFGUID guidKey, UINT32* pcbBlobSize), GetBlobSize(guidKey, pcbBlobSize))
ACT_ATTR_FWD_K(GetBlob, (REFGUID guidKey, UINT8* pBuf, UINT32 cbBufSize, UINT32* pcbBlobSize), GetBlob(guidKey, pBuf, cbBufSize, pcbBlobSize))
ACT_ATTR_FWD_K(GetAllocatedBlob, (REFGUID guidKey, UINT8** ppBuf, UINT32* pcbSize), GetAllocatedBlob(guidKey, ppBuf, pcbSize))
ACT_ATTR_FWD_K(GetUnknown, (REFGUID guidKey, REFIID riid, LPVOID* ppv), GetUnknown(guidKey, riid, ppv))
ACT_ATTR_FWD_K(SetItem, (REFGUID guidKey, REFPROPVARIANT Value), SetItem(guidKey, Value))
ACT_ATTR_FWD_K(DeleteItem, (REFGUID guidKey), DeleteItem(guidKey))
ACT_ATTR_FWD(DeleteAllItems, (), DeleteAllItems())
ACT_ATTR_FWD_K(SetUINT32, (REFGUID guidKey, UINT32 unValue), SetUINT32(guidKey, unValue))
ACT_ATTR_FWD_K(SetUINT64, (REFGUID guidKey, UINT64 unValue), SetUINT64(guidKey, unValue))
ACT_ATTR_FWD_K(SetDouble, (REFGUID guidKey, double fValue), SetDouble(guidKey, fValue))
ACT_ATTR_FWD_K(SetGUID, (REFGUID guidKey, REFGUID guidValue), SetGUID(guidKey, guidValue))
ACT_ATTR_FWD_K(SetString, (REFGUID guidKey, LPCWSTR wszValue), SetString(guidKey, wszValue))
ACT_ATTR_FWD_K(SetBlob, (REFGUID guidKey, const UINT8* pBuf, UINT32 cbSize), SetBlob(guidKey, pBuf, cbSize))
ACT_ATTR_FWD_K(SetUnknown, (REFGUID guidKey, IUnknown* pUnknown), SetUnknown(guidKey, pUnknown))
ACT_ATTR_FWD(LockStore, (), LockStore())
ACT_ATTR_FWD(UnlockStore, (), UnlockStore())
ACT_ATTR_FWD(GetCount, (UINT32* pcItems), GetCount(pcItems))
ACT_ATTR_FWD(GetItemByIndex, (UINT32 unIndex, GUID* pguidKey, PROPVARIANT* pValue), GetItemByIndex(unIndex, pguidKey, pValue))
ACT_ATTR_FWD(CopyAllItems, (IMFAttributes* pDest), CopyAllItems(pDest))
#undef ACT_ATTR_FWD_K
#undef ACT_ATTR_FWD

HRESULT CVCamActivator::FinalConstruct()
{
    VCamDiagLog(L"Act.FinalConstruct");
    HRESULT hr = MFCreateAttributes(&m_attrs, 8);
    if (FAILED(hr)) return hr;
    hr = m_attrs->SetUINT32(kVcamProvideAssociatedCameraSources, 0);
    if (FAILED(hr)) return hr;
    // Reference parity: identify the class behind the device.
    hr = m_attrs->SetGUID(kMftTransformClsidAttribute, CLSID_VCamMediaSource);
    if (FAILED(hr)) return hr;
    // 24H2 (NTDDI_WIN10_CO) flag; the frameserver reads this (missing value faulted in round 1)
    hr = m_attrs->SetUINT32(kMsCameraEffectsAttribute, 0);
    return hr;
}
