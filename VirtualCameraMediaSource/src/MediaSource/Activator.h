#pragma once
#include <atlbase.h>
#include <atlcom.h>
#include <mfidl.h>
#include <mfapi.h>
#include <mfobjects.h>
#include <dmksctrl.h>
#include <winrt/inspectable.h>

class CMediaSource;

// IMFActivate endpoint for the FrameServer: a thin object that hands out the
// real media source from ActivateObject (CatCam VirtualCamActivator pattern).
class CVCamActivator :
    public ATL::CComObjectRootEx<ATL::CComMultiThreadModelNoCS>,
    public IMFActivate,
    public IKsControl,
    public IInspectable
{
public:
    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** ppvObject) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;

    // IMFActivate
    STDMETHODIMP ActivateObject(REFIID riid, void** ppv) override;
    STDMETHODIMP ShutdownObject() override;
    STDMETHODIMP DetachObject() override;

    // IKsControl
    STDMETHODIMP KsProperty(PKSPROPERTY Property, ULONG PropertyLength, LPVOID PropertyData, ULONG DataLength, ULONG* BytesReturned) override;
    STDMETHODIMP KsMethod(PKSMETHOD Method, ULONG MethodLength, LPVOID MethodData, ULONG DataLength, ULONG* BytesReturned) override;
    STDMETHODIMP KsEvent(PKSEVENT Event, ULONG EventLength, LPVOID EventData, ULONG DataLength, ULONG* BytesReturned) override;

    // IInspectable
    STDMETHODIMP GetIids(ULONG* iidCount, IID** iids) override;
    STDMETHODIMP GetRuntimeClassName(HSTRING* className) override;
    STDMETHODIMP GetTrustLevel(TrustLevel* trustLevel) override;

    // IMFAttributes
    STDMETHODIMP GetItem(REFGUID guidKey, PROPVARIANT* pValue) override;
    STDMETHODIMP GetItemType(REFGUID guidKey, MF_ATTRIBUTE_TYPE* pType) override;
    STDMETHODIMP CompareItem(REFGUID guidKey, REFPROPVARIANT Value, BOOL* pbResult) override;
    STDMETHODIMP Compare(IMFAttributes* pTheirs, MF_ATTRIBUTES_MATCH_TYPE MatchType, BOOL* pbResult) override;
    STDMETHODIMP GetUINT32(REFGUID guidKey, UINT32* punValue) override;
    STDMETHODIMP GetUINT64(REFGUID guidKey, UINT64* punValue) override;
    STDMETHODIMP GetDouble(REFGUID guidKey, double* pfValue) override;
    STDMETHODIMP GetGUID(REFGUID guidKey, GUID* pguidValue) override;
    STDMETHODIMP GetStringLength(REFGUID guidKey, UINT32* pcchLength) override;
    STDMETHODIMP GetString(REFGUID guidKey, LPWSTR pwszValue, UINT32 cchBufSize, UINT32* pcchLength) override;
    STDMETHODIMP GetAllocatedString(REFGUID guidKey, LPWSTR* ppwszValue, UINT32* pcchLength) override;
    STDMETHODIMP GetBlobSize(REFGUID guidKey, UINT32* pcbBlobSize) override;
    STDMETHODIMP GetBlob(REFGUID guidKey, UINT8* pBuf, UINT32 cbBufSize, UINT32* pcbBlobSize) override;
    STDMETHODIMP GetAllocatedBlob(REFGUID guidKey, UINT8** ppBuf, UINT32* pcbSize) override;
    STDMETHODIMP GetUnknown(REFGUID guidKey, REFIID riid, LPVOID* ppv) override;
    STDMETHODIMP SetItem(REFGUID guidKey, REFPROPVARIANT Value) override;
    STDMETHODIMP DeleteItem(REFGUID guidKey) override;
    STDMETHODIMP DeleteAllItems() override;
    STDMETHODIMP SetUINT32(REFGUID guidKey, UINT32 unValue) override;
    STDMETHODIMP SetUINT64(REFGUID guidKey, UINT64 unValue) override;
    STDMETHODIMP SetDouble(REFGUID guidKey, double fValue) override;
    STDMETHODIMP SetGUID(REFGUID guidKey, REFGUID guidValue) override;
    STDMETHODIMP SetString(REFGUID guidKey, LPCWSTR wszValue) override;
    STDMETHODIMP SetBlob(REFGUID guidKey, const UINT8* pBuf, UINT32 cbSize) override;
    STDMETHODIMP SetUnknown(REFGUID guidKey, IUnknown* pUnknown) override;
    STDMETHODIMP LockStore() override;
    STDMETHODIMP UnlockStore() override;
    STDMETHODIMP GetCount(UINT32* pcItems) override;
    STDMETHODIMP GetItemByIndex(UINT32 unIndex, GUID* pguidKey, PROPVARIANT* pValue) override;
    STDMETHODIMP CopyAllItems(IMFAttributes* pDest) override;

    CVCamActivator();

    HRESULT FinalConstruct();

private:
    ~CVCamActivator();

    CMediaSource* m_source = nullptr;  // owned (one ref held)
    IMFAttributes* m_attrs = nullptr;  // owned
};
