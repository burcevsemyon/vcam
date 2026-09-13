#pragma once
#include <atlbase.h>
#include <atlcom.h>
#include <mfidl.h>
#include <mfapi.h>
#include <mfobjects.h>
#include <dmksctrl.h>
#include "GUIDs.h"
#include <winrt/inspectable.h>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <deque>
#include "SharedMemoryFrameSource.h"
#include "AttrLogProxy.h"

// Shared (cross-session) video sample allocator. Some desktop SDK partitions do
// not expose IMFVideoSampleAllocator; provide a local mirror that is skipped
// when the SDK already defines it (the interface-defined guard macro).
#ifndef __IMFVideoSampleAllocator_INTERFACE_DEFINED__
#define __IMFVideoSampleAllocator_INTERFACE_DEFINED__
MIDL_INTERFACE("86cbc910-e533-4751-8e3b-f19b5b806a03")
struct IMFVideoSampleAllocator : public IUnknown
{
    virtual HRESULT STDMETHODCALLTYPE SetDirectXManager(IUnknown* pManager) = 0;
    virtual HRESULT STDMETHODCALLTYPE UninitializeSampleAllocator(void) = 0;
    virtual HRESULT STDMETHODCALLTYPE InitializeSampleAllocator(DWORD cRequestedFrames, IMFMediaType* pMediaType) = 0;
    virtual HRESULT STDMETHODCALLTYPE AllocateSample(IMFSample** ppSample) = 0;
};
#endif

class CMediaSource;

class CMediaStream :
    public ATL::CComObjectRootEx<ATL::CComMultiThreadModelNoCS>,
    public IMFMediaStream2, // linear chain: IMFMediaStream2 : IMFMediaStream : IMFMediaEventGenerator
    public IInspectable
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

    // IMFMediaStream
    STDMETHODIMP GetMediaSource(IMFMediaSource** ppSource) override;
    STDMETHODIMP GetStreamDescriptor(IMFStreamDescriptor** ppDescriptor) override;
    STDMETHODIMP RequestSample(IUnknown* pToken) override;

    // IMFMediaStream2
    STDMETHODIMP SetStreamState(MF_STREAM_STATE state) override;
    STDMETHODIMP GetStreamState(MF_STREAM_STATE* pState) override;

    // IInspectable
    STDMETHODIMP GetIids(ULONG* iidCount, IID** iids) override;
    STDMETHODIMP GetRuntimeClassName(HSTRING* className) override;
    STDMETHODIMP GetTrustLevel(TrustLevel* trustLevel) override;

    HRESULT FinalConstruct(CMediaSource* pSource);
    HRESULT StartForSession();
    HRESULT StopForSession();
    void ShutDownInternal();
    HRESULT SetMediaType(IMFMediaType* pMediaType);
    HRESULT SetAllocator(IUnknown* pAllocator);

    // Returns the (logging) attributes proxy with an added reference; caller must Release.
    IMFAttributes* GetStreamAttributes()
    {
        if (m_pStreamAttrsProxy != nullptr) {
            m_pStreamAttrsProxy->AddRef();
            return m_pStreamAttrsProxy;
        }
        return m_pStreamAttributes;
    }

private:
    ~CMediaStream();
    void WorkerMain();
    HRESULT DeliverNextSample(IUnknown* pToken);

    CMediaSource* m_pSource = nullptr;          // not owned (source owns stream)
    IMFMediaEventQueue* m_pEventQueue = nullptr; // owned
    IMFStreamDescriptor* m_pStreamDescriptor = nullptr; // owned
    IMFMediaType* m_pMediaType = nullptr;          // owned
    IMFMediaType* m_pMediaTypeNv12 = nullptr;      // owned (secondary NV12 format)
    IMFAttributes* m_pStreamAttributes = nullptr;  // owned
    CAttrLogProxy* m_pStreamAttrsProxy = nullptr;  // owned (TEMP DIAGNOSTIC)
    IMFVideoSampleAllocator* m_pAllocator = nullptr; // owned (shared cross-session allocator from SetDefaultAllocator)
    BYTE* m_pNv12Scratch = nullptr;                  // owned, lazy (RGB32 staging buffer for NV12 conversion)
    bool m_selectedNv12 = false;                     // NV12 negotiated (SetMediaType / SD handler)

    MF_STREAM_STATE m_state = MF_STREAM_STATE_STOPPED;
    bool m_shutdown = false;

    std::mutex m_tokenMutex;
    std::condition_variable m_tokenCv;
    std::deque<IUnknown*> m_tokens;
    std::thread m_worker;
    bool m_workerStop = false;

    CRITICAL_SECTION m_cs;
    bool m_csInit = false;
};
