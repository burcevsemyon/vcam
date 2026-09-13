#pragma once
#include <atlbase.h>
#include <atlcom.h>
#include <mfidl.h>
#include <mfapi.h>
#include <mfobjects.h>
#include <dmksctrl.h>
#include "GUIDs.h"
#include "SampleAllocatorControl.h"
#include "AttrLogProxy.h"
#include <winrt/inspectable.h>

class CMediaStream;
class CVCamActivator;

class CMediaSource :
    public ATL::CComObjectRootEx<ATL::CComMultiThreadModelNoCS>,
    public IMFMediaSource2, // linear chain: IMFMediaSource2 : IMFMediaSourceEx : IMFMediaSource : IMFMediaEventGenerator
    public IMFGetService,
    public IMFRealTimeClientEx,
    public IKsControl,
    public IInspectable,
    public IMFSampleAllocatorControl
{
public:
    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** ppvObject) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;

    // IMFMediaEventGenerator
    STDMETHODIMP GetEvent(DWORD dwFlags, IMFMediaEvent** ppEvent) override;
    STDMETHODIMP BeginGetEvent(IMFAsyncCallback* pCallback, IUnknown* punkState) override;
    STDMETHODIMP EndGetEvent(IMFAsyncResult* pResult, IMFMediaEvent** ppEvent) override;
    STDMETHODIMP QueueEvent(MediaEventType met, REFGUID guidExtendedType, HRESULT hrStatus, const PROPVARIANT* pvValue) override;

    // IMFMediaSource
    STDMETHODIMP GetCharacteristics(DWORD* pdwCharacteristics) override;
    STDMETHODIMP CreatePresentationDescriptor(IMFPresentationDescriptor** ppDesc) override;
    STDMETHODIMP Start(IMFPresentationDescriptor* pPresentationDescriptor, const GUID* pguidTimeFormat, const PROPVARIANT* pvarStartPosition) override;
    STDMETHODIMP Stop() override;
    STDMETHODIMP Pause() override;
    STDMETHODIMP Shutdown() override;

    // IMFMediaSourceEx
    STDMETHODIMP GetSourceAttributes(IMFAttributes** ppAttributes) override;
    STDMETHODIMP GetStreamAttributes(DWORD dwStreamIdentifier, IMFAttributes** ppAttributes) override;
    STDMETHODIMP SetD3DManager(IUnknown* pManager) override;

    // IMFMediaSource2
    STDMETHODIMP SetMediaType(DWORD dwStreamID, IMFMediaType* pMediaType) override;

    // Look up a stream by its MF_DEVICESTREAM_STREAM_ID (not part of the local IMFMediaSource2).
    STDMETHODIMP GetStreamByStreamID(LPGUID pguidStreamID, IMFMediaStream** ppStream);

    // IMFGetService
    STDMETHODIMP GetService(REFGUID rSID, REFIID riid, void** ppv) override;

    // IMFRealTimeClientEx
    STDMETHODIMP RegisterThreadsEx(DWORD* pdwTaskIndex, LPCWSTR wszClassName, LONG lBasePriority) override;
    STDMETHODIMP UnregisterThreads() override;
    STDMETHODIMP SetWorkQueueEx(DWORD dwMultithreadedWorkQueueId, LONG lWorkItemBasePriority) override;

    // IKsControl
    STDMETHODIMP KsProperty(PKSPROPERTY Property, ULONG PropertyLength, LPVOID PropertyData, ULONG DataLength, ULONG* BytesReturned) override;
    STDMETHODIMP KsMethod(PKSMETHOD Method, ULONG MethodLength, LPVOID MethodData, ULONG DataLength, ULONG* BytesReturned) override;
    STDMETHODIMP KsEvent(PKSEVENT Event, ULONG EventLength, LPVOID EventData, ULONG DataLength, ULONG* BytesReturned) override;

    // IInspectable
    STDMETHODIMP GetIids(ULONG* iidCount, IID** iids) override;
    STDMETHODIMP GetRuntimeClassName(HSTRING* className) override;
    STDMETHODIMP GetTrustLevel(TrustLevel* trustLevel) override;

    // IMFSampleAllocatorControl
    STDMETHODIMP SetDefaultAllocator(DWORD dwOutputStreamID, IUnknown* pAllocator) override;
    STDMETHODIMP GetAllocatorUsage(DWORD dwOutputStreamID, DWORD* pdwInputStreamID, MFSampleAllocatorUsage* peUsage) override;

    HRESULT FinalConstruct();

    // Copy the activator's attributes into the source store (parity with smourier/VCamSample).
    HRESULT CopyActivationAttributes(IMFAttributes* pActivatorAttrs);

    // Source-specific attributes. Must run AFTER CopyActivationAttributes:
    // CopyAllItems replaces the destination store's contents.
    HRESULT SetIntrinsicAttributes();

private:
    ~CMediaSource();

    CMediaStream* m_pStream = nullptr;           // owned
    IMFMediaEventQueue* m_pEventQueue = nullptr; // owned
    IMFAttributes* m_pSourceAttrs = nullptr;     // owned
    CAttrLogProxy* m_pSourceAttrsProxy = nullptr; // owned (TEMP DIAGNOSTIC)
    DWORD m_characteristics = MFMEDIASOURCE_IS_LIVE | MFMEDIASOURCE_DOES_NOT_USE_NETWORK;
    bool m_shutdown = false;
    bool m_started = false;
};
