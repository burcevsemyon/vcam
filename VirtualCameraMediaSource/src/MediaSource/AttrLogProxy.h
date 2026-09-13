#pragma once
#include <windows.h>
#include <mfidl.h>
#include <mfobjects.h>

// TEMP DIAGNOSTIC - declarations live in dllmain.cpp. Remove before shipping.
void VCamDiagLog(const wchar_t* fmt, ...);
void VCamDiagGuid(const GUID* g, wchar_t* out, size_t cch);

// TEMP DIAGNOSTIC - logging wrapper around an IMFAttributes store.
// Records every attribute access the frameserver performs (key GUID + HRESULT)
// so a fatal MF_E_ATTRIBUTENOTFOUND can be pinpointed. Remove before shipping.
class CAttrLogProxy final : public IMFAttributes
{
public:
    CAttrLogProxy(IMFAttributes* pInner, const wchar_t* tag)
        : m_pInner(pInner), m_tag(tag), m_ref(1)
    {
        if (m_pInner) m_pInner->AddRef();
    }

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override
    {
        if (ppv == nullptr) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IMFAttributes) {
            *ppv = static_cast<IMFAttributes*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override
    {
        return static_cast<ULONG>(InterlockedIncrement(&m_ref));
    }
    STDMETHODIMP_(ULONG) Release() override
    {
        LONG r = InterlockedDecrement(&m_ref);
        if (r == 0) {
            if (m_pInner) m_pInner->Release();
            delete this;
        }
        return static_cast<ULONG>(r);
    }

private:
    void LogKey(REFGUID guidKey, const wchar_t* method, HRESULT hr)
    {
        wchar_t k[64];
        VCamDiagGuid(&guidKey, k, 64);
        VCamDiagLog(L"%s.attr.%s %s -> 0x%08X", m_tag, method, k, hr);
    }
    void LogRiid(REFGUID guidKey, REFIID riid, const wchar_t* method, HRESULT hr)
    {
        wchar_t k[64];
        wchar_t r[64];
        VCamDiagGuid(&guidKey, k, 64);
        VCamDiagGuid(&riid, r, 64);
        VCamDiagLog(L"%s.attr.%s %s riid=%s -> 0x%08X", m_tag, method, k, r, hr);
    }
    void LogPlain(const wchar_t* method, HRESULT hr)
    {
        VCamDiagLog(L"%s.attr.%s -> 0x%08X", m_tag, method, hr);
    }

#define AAL_K(name, args, call) \
    STDMETHODIMP name args override \
    { \
        HRESULT _hr = m_pInner ? m_pInner->call : E_NOTIMPL; \
        LogKey(guidKey, L"" #name, _hr); \
        return _hr; \
    }
#define AAL_P(name, args, call) \
    STDMETHODIMP name args override \
    { \
        HRESULT _hr = m_pInner ? m_pInner->call : E_NOTIMPL; \
        LogPlain(L"" #name, _hr); \
        return _hr; \
    }

    // IMFAttributes - every method forwards to the inner store and is logged
    AAL_K(GetItem, (REFGUID guidKey, PROPVARIANT* pValue), GetItem(guidKey, pValue))
    AAL_K(GetItemType, (REFGUID guidKey, MF_ATTRIBUTE_TYPE* pType), GetItemType(guidKey, pType))
    AAL_K(CompareItem, (REFGUID guidKey, REFPROPVARIANT Value, BOOL* pbResult), CompareItem(guidKey, Value, pbResult))
    AAL_P(Compare, (IMFAttributes* pTheirs, MF_ATTRIBUTES_MATCH_TYPE MatchType, BOOL* pbResult), Compare(pTheirs, MatchType, pbResult))
    AAL_K(GetUINT32, (REFGUID guidKey, UINT32* punValue), GetUINT32(guidKey, punValue))
    AAL_K(GetUINT64, (REFGUID guidKey, UINT64* punValue), GetUINT64(guidKey, punValue))
    AAL_K(GetDouble, (REFGUID guidKey, double* pfValue), GetDouble(guidKey, pfValue))
    AAL_K(GetGUID, (REFGUID guidKey, GUID* pguidValue), GetGUID(guidKey, pguidValue))
    AAL_K(GetStringLength, (REFGUID guidKey, UINT32* pcchLength), GetStringLength(guidKey, pcchLength))
    AAL_K(GetString, (REFGUID guidKey, LPWSTR pwszValue, UINT32 cchBufSize, UINT32* pcchLength), GetString(guidKey, pwszValue, cchBufSize, pcchLength))
    AAL_K(GetAllocatedString, (REFGUID guidKey, LPWSTR* ppwszValue, UINT32* pcchLength), GetAllocatedString(guidKey, ppwszValue, pcchLength))
    AAL_K(GetBlobSize, (REFGUID guidKey, UINT32* pcbBlobSize), GetBlobSize(guidKey, pcbBlobSize))
    AAL_K(GetBlob, (REFGUID guidKey, UINT8* pBuf, UINT32 cbBufSize, UINT32* pcbBlobSize), GetBlob(guidKey, pBuf, cbBufSize, pcbBlobSize))
    AAL_K(GetAllocatedBlob, (REFGUID guidKey, UINT8** ppBuf, UINT32* pcbSize), GetAllocatedBlob(guidKey, ppBuf, pcbSize))
    AAL_K(SetItem, (REFGUID guidKey, REFPROPVARIANT Value), SetItem(guidKey, Value))
    AAL_K(DeleteItem, (REFGUID guidKey), DeleteItem(guidKey))
    AAL_P(DeleteAllItems, (), DeleteAllItems())
    AAL_K(SetUINT32, (REFGUID guidKey, UINT32 unValue), SetUINT32(guidKey, unValue))
    AAL_K(SetUINT64, (REFGUID guidKey, UINT64 unValue), SetUINT64(guidKey, unValue))
    AAL_K(SetDouble, (REFGUID guidKey, double fValue), SetDouble(guidKey, fValue))
    AAL_K(SetGUID, (REFGUID guidKey, REFGUID guidValue), SetGUID(guidKey, guidValue))
    AAL_K(SetString, (REFGUID guidKey, LPCWSTR wszValue), SetString(guidKey, wszValue))
    AAL_K(SetBlob, (REFGUID guidKey, const UINT8* pBuf, UINT32 cbSize), SetBlob(guidKey, pBuf, cbSize))
    AAL_K(SetUnknown, (REFGUID guidKey, IUnknown* pUnknown), SetUnknown(guidKey, pUnknown))
    AAL_P(LockStore, (), LockStore())
    AAL_P(UnlockStore, (), UnlockStore())
    AAL_P(GetCount, (UINT32* pcItems), GetCount(pcItems))
    AAL_P(GetItemByIndex, (UINT32 unIndex, GUID* pguidKey, PROPVARIANT* pValue), GetItemByIndex(unIndex, pguidKey, pValue))
    AAL_P(CopyAllItems, (IMFAttributes* pDest), CopyAllItems(pDest))

#undef AAL_K
#undef AAL_P

    STDMETHODIMP GetUnknown(REFGUID guidKey, REFIID riid, LPVOID* ppv) override
    {
        HRESULT _hr = m_pInner ? m_pInner->GetUnknown(guidKey, riid, ppv) : E_NOTIMPL;
        LogRiid(guidKey, riid, L"GetUnknown", _hr);
        return _hr;
    }

    IMFAttributes* m_pInner;    // owned
    const wchar_t* m_tag;
    volatile LONG m_ref;
};
