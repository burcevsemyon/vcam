#pragma once
#include <atlbase.h>
#include <atlcom.h>
#include <mfidl.h>
#include <mfapi.h>
#include <mfobjects.h>
#include <dmksctrl.h>
#include "GUIDs.h"
#include <inspectable.h>
#include <thread>
#include <mutex>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include "SharedMemoryFrameSource.h"
#include "V2FrameReader.h"
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

    CMediaStream();

    HRESULT FinalConstruct(CMediaSource* pSource);
    HRESULT StartForSession();
    HRESULT StopForSession();
    void ShutDownInternal();
    HRESULT SetMediaType(IMFMediaType* pMediaType);
    HRESULT SetAllocator(IUnknown* pAllocator);

    // Returns the (logging) attributes store with an added reference; caller must Release.
    IMFAttributes* GetStreamAttributes()
    {
        IMFAttributes* pAttrs = (m_pStreamAttrsProxy.p != nullptr)
            ? static_cast<IMFAttributes*>(m_pStreamAttrsProxy.p) : m_pStreamAttributes.p;
        if (pAttrs) pAttrs->AddRef(); // COM contract: caller releases
        return pAttrs;
    }

private:
    ~CMediaStream();
    void WorkerMain();
    HRESULT DeliverNextSample(IUnknown* pToken);
    // Resolves the negotiated type: SD handler current type (authoritative —
    // the frameserver proxy may set it directly) with fallback to m_selected*.
    // The handler pointer is cached in FinalConstruct (the descriptor never
    // changes), but the current type is re-read live on every call: external
    // writers (proxy / direct SetCurrentMediaType on the handler) are not
    // observable, so the resolved result itself must NOT be cached.
    void ResolveNegotiatedType(UINT32* pW, UINT32* pH, bool* pNv12) const;
    // v2-доставка: AcquireV2Frame тянет свежий валидный v2-кадр в m_pV2Staging
    // (realloc под диметры); DeliverFromV2 раскладывает его в pBits цели
    // (натив 1:1, 720/640 letterbox-fit, NV12-720 через скретч). false в обоих —
    // вызывающий идёт СТАРЫМ v1-путём бит-в-бит.
    bool AcquireV2Frame(UINT32* pW, UINT32* pH, UINT32* pStride);
    bool DeliverFromV2(BYTE* pBits, UINT32 w, UINT32 h, bool useNv12,
                       UINT32 vw, UINT32 vh, UINT32 vstride);
    HRESULT DeliverLocalSample(BYTE*& pBits, ATL::CComPtr<IMFMediaBuffer>& pBuffer,
                               ATL::CComPtr<IMFSample>& pSample, UINT32 w, UINT32 h, bool useNv12,
                               bool haveV2, UINT32 v2w, UINT32 v2h, UINT32 v2stride,
                               UINT32 rgbBytes, UINT32 nv12Bytes);
    HRESULT DeliverSharedSample(BYTE* pBits, ATL::CComPtr<IMFMediaBuffer>& pBuffer,
                                UINT32 w, UINT32 h, bool useNv12,
                                bool haveV2, UINT32 v2w, UINT32 v2h, UINT32 v2stride,
                                DWORD needed, DWORD maxLen);

    CMediaSource* m_pSource = nullptr;          // not owned (source owns stream)
    ATL::CComPtr<IMFMediaEventQueue> m_pEventQueue; // owned
    ATL::CComPtr<IMFStreamDescriptor> m_pStreamDescriptor; // owned
    ATL::CComPtr<IMFMediaTypeHandler> m_pTypeHandler;      // owned (handler of m_pStreamDescriptor; fixed in FinalConstruct)
    ATL::CComPtr<IMFMediaType> m_pMediaType;          // owned (1280x720 RGB32)
    ATL::CComPtr<IMFMediaType> m_pMediaTypeNv12;      // owned (1280x720 NV12)
    ATL::CComPtr<IMFMediaType> m_pMediaType640;       // owned (640x480 RGB32)
    // Натив v2 (w×h RGB32 из живого заголовка v2): только когда рекламируется
    // (валиден и != 720p); иначе nullptr и лесенка = старые 3 типа.
    ATL::CComPtr<IMFMediaType> m_pMediaTypeNative;    // owned (nullable)
    UINT32 m_nativeW = 0; // рекламируемые натив-диметры (0 = не рекламируется)
    UINT32 m_nativeH = 0;
    ATL::CComPtr<IMFAttributes> m_pStreamAttributes;  // owned
    ATL::CComPtr<CAttrLogProxy> m_pStreamAttrsProxy;  // owned (TEMP DIAGNOSTIC)
    ATL::CComPtr<IMFVideoSampleAllocator> m_pAllocator; // owned (shared cross-session allocator from SetDefaultAllocator)
    std::unique_ptr<BYTE[]> m_pNv12Scratch;       // owned, lazy (RGB32 staging buffer for NV12 conversion)
    mutable vcam_v2::V2Reader m_v2;               // owned (read-only v2, Global->Local; mutable: resolve в const)
    std::unique_ptr<BYTE[]> m_pV2Staging;          // owned, lazy (v2 frame, native stride)
    SIZE_T m_cbV2Staging = 0;
    UINT32 m_selectedWidth = vcam::VCamWidth;        // negotiated width
    UINT32 m_selectedHeight = vcam::VCamHeight;      // negotiated height
    bool m_selectedNv12 = false;                     // NV12 negotiated (SetMediaType / SD handler)
    UINT32 m_lastDeliverW = 0;                       // last logged delivered size (diag dedupe)
    UINT32 m_lastDeliverH = 0;
    bool m_lastDeliverNv12 = false;
    // Троттлинг per-frame DiagEvent (perf-review): без гарда строки ниже писались
    // 30/с в файл. Сбрасываются в StartForSession (per-session).
    bool m_loggedNoV2 = false;            // "no v2" уже залогировано в сессии
    ULONGLONG m_lastLockLogTick = 0;      // последний Lock-лог (rate-limit 1/с)
    ULONGLONG m_lastTooSmallTick = 0;     // последний "buffer too small" (rate-limit 1/с)

    MF_STREAM_STATE m_state = MF_STREAM_STATE_STOPPED;
    std::atomic<bool> m_shutdown{false}; // read from client threads, set by shutdown

    std::mutex m_tokenMutex;
    std::condition_variable m_tokenCv;
    std::deque<ATL::CComPtr<IUnknown>> m_tokens;
    std::thread m_worker;
    bool m_workerStop = false;

    mutable ATL::CComAutoCriticalSection m_cs;
};
