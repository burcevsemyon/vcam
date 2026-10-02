#include "VideoProcessorScaler.h"

#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mferror.h>
#include <atlbase.h>

#include <cmath>
#include <cstring>
#include <mutex>
#include <string>

#include "SharedMemoryContract.h"

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")

namespace {

const UINT kOutW = vcam::VCamWidth;    // 1280
const UINT kOutH = vcam::VCamHeight;   // 720
const LONG kOutStride = (LONG)vcam::VCamStride; // 5120
const LONGLONG kFrameDuration = 333333; // 30 FPS в 100ns

void LogMft(const std::wstring& msg)
{
    OutputDebugStringW((L"[ProducerCore:mft] " + msg + L"\n").c_str());
}

std::wstring HrHex(HRESULT hr)
{
    wchar_t buf[16];
    swprintf(buf, 16, L"0x%08X", (unsigned)hr);
    return std::wstring(buf);
}

bool IsPackedRgb(const GUID& sub)
{
    return IsEqualGUID(sub, MFVideoFormat_RGB32) ||
           IsEqualGUID(sub, MFVideoFormat_ARGB32);
}

// Метрики packed-буфера, который мы отдаём MFT (MFT всегда видит
// contiguous top-down; исходный stride учитывается только при копировании).
bool PackedMetrics(const GUID& sub, UINT w, UINT h, LONG stride,
                   LONG& packedStride, DWORD& bufSize)
{
    if (w == 0 || h == 0 || w > 8192 || h > 8192 || stride == 0)
        return false;
    if (IsPackedRgb(sub)) {
        LONG row = (LONG)(w * 4);
        if (row <= 0 || (stride > 0 ? stride < row : -stride < row))
            return false;
        packedStride = row;
        bufSize = (DWORD)((size_t)row * h);
        return true;
    }
    if (IsEqualGUID(sub, MFVideoFormat_YUY2)) {
        LONG row = (LONG)(w * 2);
        if (row <= 0 || (stride > 0 ? stride < row : -stride < row))
            return false;
        packedStride = row;
        bufSize = (DWORD)((size_t)row * h);
        return true;
    }
    if (IsEqualGUID(sub, MFVideoFormat_NV12)) {
        if (h % 2 != 0 || stride <= 0 || stride < (LONG)w)
            return false;
        packedStride = (LONG)w;
        bufSize = (DWORD)((size_t)w * h * 3 / 2);
        return true;
    }
    return false;
}

void CopyRowsTopDown(BYTE* dst, LONG dstStride, const BYTE* src, LONG srcStride,
                     UINT rowBytes, UINT h)
{
    for (UINT y = 0; y < h; y++) {
        const BYTE* srow = (srcStride >= 0)
            ? src + (size_t)y * (size_t)srcStride
            : src + (size_t)(h - 1 - y) * (size_t)(-srcStride);
        memcpy(dst + (size_t)y * (size_t)dstStride, srow, rowBytes);
    }
}

} // namespace

VideoProcessorScaler::VideoProcessorScaler()
    : mutex_(new std::mutex())
{
}

VideoProcessorScaler::~VideoProcessorScaler()
{
    std::lock_guard<std::mutex> lock(*(std::mutex*)mutex_);
    TeardownLocked();
    void* m = mutex_;
    mutex_ = nullptr;
    delete (std::mutex*)m;
}

void VideoProcessorScaler::Shutdown()
{
    std::lock_guard<std::mutex> lock(*(std::mutex*)mutex_);
    TeardownLocked();
}

void VideoProcessorScaler::TeardownLocked()
{
    // Порядок важен: сначала сбросить pending-состояние MFT (FLUSH), потом
    // закрыть стрим и отпустить COM-ссылки — всё пока жива платформа MF.
    // Без FLUSH teardown с необработанным входом роняет процесс на выходе
    // (замерено: AV в CRT-teardown после CoUninitialize).
    IMFTransform* mft = (IMFTransform*)mft_;
    if (mft) {
        if (streaming_) {
            mft->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
            mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
        }
        mft->Release();
        mft_ = nullptr;
    }
    if (ctrl_) {
        ((IMFVideoProcessorControl*)ctrl_)->Release();
        ctrl_ = nullptr;
    }
    streaming_ = false;
    cfgW_ = cfgH_ = 0;
    cfgSubtype_ = GUID_NULL;
    outStride_ = 0;
    outBufSize_ = 0;
    frameIndex_ = 0;
}

bool VideoProcessorScaler::Scale(const BYTE* src, UINT w, UINT h, LONG stride, BYTE* dst)
{
    return ScaleEx(src, w, h, stride, MFVideoFormat_RGB32, dst);
}

bool VideoProcessorScaler::ScaleEx(const BYTE* src, UINT w, UINT h, LONG stride,
                                   const GUID& subtype, BYTE* dst)
{
    if (!src || !dst || w == 0 || h == 0)
        return false;
    LONG packedStride = 0;
    DWORD inSize = 0;
    if (!PackedMetrics(subtype, w, h, stride, packedStride, inSize))
        return false;

    std::lock_guard<std::mutex> lock(*(std::mutex*)mutex_);
    if (!EnsureInit())
        return false;
    if (!Configure(w, h, subtype))
        return false;
    return Process(src, stride, subtype, dst);
}

bool VideoProcessorScaler::EnsureInit()
{
    if (mft_)
        return true;
    if (unavailable_)
        return false;

    IMFTransform* mft = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_VideoProcessorMFT, nullptr,
                                  CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&mft));
    if (FAILED(hr) || !mft) {
        // COM может быть не инициализирован на этом потоке (Render хоста) —
        // не poison'им навсегда, следующий кадр попробует снова.
        LogMft(L"VideoProcessorMFT unavailable: " + HrHex(hr));
        return false;
    }

    IMFVideoProcessorControl* ctrl = nullptr;
    hr = mft->QueryInterface(IID_PPV_ARGS(&ctrl));
    if (FAILED(hr) || !ctrl) {
        LogMft(L"IMFVideoProcessorControl QI failed: " + HrHex(hr));
        mft->Release();
        unavailable_ = true; // без rect-контроля letterbox неверный — только CPU
        return false;
    }
    mft_ = mft;
    ctrl_ = ctrl;

    // Software-режим: D3D-менеджер не аттачим сознательно. Замерено: VP MFT
    // с D3D-менеджером требует surface-сэмплы и отклоняет sysmem-вход
    // E_NOINTERFACE на ProcessInput. GPU-путь потребовал бы DXGI
    // upload/download — вне скоупа; software MFT делает convert+scale сам.
    return true;
}

bool VideoProcessorScaler::Configure(UINT w, UINT h, const GUID& subtype)
{
    if (cfgW_ == w && cfgH_ == h && IsEqualGUID(cfgSubtype_, subtype) && mft_)
        return true;

    IMFTransform* mft = (IMFTransform*)mft_;
    IMFVideoProcessorControl* ctrl = (IMFVideoProcessorControl*)ctrl_;
    if (!mft || !ctrl)
        return false;

    ATL::CComPtr<IMFMediaType> inType;
    if (FAILED(MFCreateMediaType(&inType)) || !inType)
        return false;
    inType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    inType->SetGUID(MF_MT_SUBTYPE, subtype);
    inType->SetUINT64(MF_MT_FRAME_SIZE, ((UINT64)w << 32) | (UINT64)h);
    inType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    inType->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
    // Входной fps = выходному (30): иначе MFT входит в режим rate-conversion
    // и буферизует кадры (замерено: каждый 2-й ProcessInput -> NEED_MORE_INPUT).
    inType->SetUINT64(MF_MT_FRAME_RATE, ((UINT64)30 << 32) | 1ULL);
    HRESULT hr = mft->SetInputType(0, inType, 0);
    inType = nullptr;
    if (FAILED(hr)) {
        LogMft(L"SetInputType failed: " + HrHex(hr));
        return false;
    }

    ATL::CComPtr<IMFMediaType> outType;
    if (FAILED(MFCreateMediaType(&outType)) || !outType)
        return false;
    outType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    outType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
    outType->SetUINT64(MF_MT_FRAME_SIZE, ((UINT64)kOutW << 32) | (UINT64)kOutH);
    outType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    outType->SetUINT64(MF_MT_FRAME_RATE, ((UINT64)30 << 32) | 1ULL);
    hr = mft->SetOutputType(0, outType, 0);
    outType = nullptr;
    if (FAILED(hr)) {
        LogMft(L"SetOutputType(RGB32 1280x720) failed: " + HrHex(hr));
        return false;
    }

    // Фактический stride выхода (MFT может паддить строку).
    LONG stride = 0;
    ATL::CComPtr<IMFMediaType> cur;
    if (SUCCEEDED(mft->GetOutputCurrentType(0, &cur)) && cur) {
        UINT32 s = 0;
        if (SUCCEEDED(cur->GetUINT32(MF_MT_DEFAULT_STRIDE, &s)))
            stride = (LONG)(INT32)s;
        cur = nullptr;
    }
    outStride_ = (stride != 0) ? stride : kOutStride;

    MFT_OUTPUT_STREAM_INFO osi = {};
    if (SUCCEEDED(mft->GetOutputStreamInfo(0, &osi)) && osi.cbSize)
        outBufSize_ = (unsigned long)osi.cbSize;
    else
        outBufSize_ = 0;
    if (outBufSize_ < (unsigned long)kOutStride * kOutH)
        outBufSize_ = (unsigned long)kOutStride * kOutH;

    // Letterbox: fit с сохранением пропорций, центр, поля — чёрные.
    double scale = (double)kOutW / w;
    double s2 = (double)kOutH / h;
    if (s2 < scale) scale = s2;
    int dw = (int)llround((double)w * scale);
    int dh = (int)llround((double)h * scale);
    if (dw < 1) dw = 1;
    if (dh < 1) dh = 1;
    if (dw > (int)kOutW) dw = (int)kOutW;
    if (dh > (int)kOutH) dh = (int)kOutH;
    int x0 = ((int)kOutW - dw) / 2;
    int y0 = ((int)kOutH - dh) / 2;
    RECT rc = { x0, y0, x0 + dw, y0 + dh };

    hr = ctrl->SetSourceRectangle(nullptr); // весь входной кадр
    if (FAILED(hr)) {
        LogMft(L"SetSourceRectangle failed: " + HrHex(hr));
        return false;
    }
    hr = ctrl->SetDestinationRectangle(&rc);
    if (FAILED(hr)) {
        LogMft(L"SetDestinationRectangle failed: " + HrHex(hr));
        return false;
    }
    MFARGB black = { 0, 0, 0, 255 }; // B,G,R,A — непрозрачный чёрный
    hr = ctrl->SetBorderColor(&black);
    if (FAILED(hr)) {
        LogMft(L"SetBorderColor failed: " + HrHex(hr));
        return false;
    }

    if (!streaming_) {
        mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
        streaming_ = true;
    } else {
        mft->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
    }

    cfgW_ = w;
    cfgH_ = h;
    cfgSubtype_ = subtype;
    frameIndex_ = 0; // новый стрим — таймстампы с нуля
    return true;
}

bool VideoProcessorScaler::Process(const BYTE* src, LONG stride, const GUID& subtype, BYTE* dst)
{
    // MFT держим stateless 1:1: после каждого успеха — FLUSH (типы/rect'ы
    // сохраняются). Без этого pending-вход роняет процесс на teardown'е, а
    // каждый 2-й кадр упирается в NEED_MORE_INPUT (замерено).
    bool rejected = false;
    if (ProcessOnce(src, stride, subtype, dst, rejected))
        return true;
    if (!rejected)
        return false;
    IMFTransform* mft = (IMFTransform*)mft_;
    if (mft && streaming_)
        mft->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
    rejected = false;
    return ProcessOnce(src, stride, subtype, dst, rejected);
}

bool VideoProcessorScaler::ProcessOnce(const BYTE* src, LONG stride, const GUID& subtype,
                                       BYTE* dst, bool& rejected)
{
    rejected = false;
    IMFTransform* mft = (IMFTransform*)mft_;
    if (!mft)
        return false;

    LONG packedStride = 0;
    DWORD inSize = 0;
    if (!PackedMetrics(subtype, cfgW_, cfgH_, stride, packedStride, inSize))
        return false;

    // Входной сэмпл: contiguous top-down packed.
    ATL::CComPtr<IMFMediaBuffer> inBuf;
    if (FAILED(MFCreateMemoryBuffer(inSize, &inBuf)) || !inBuf)
        return false;
    BYTE* wptr = nullptr;
    DWORD maxLen = 0;
    if (FAILED(inBuf->Lock(&wptr, &maxLen, nullptr)) || !wptr || maxLen < inSize) {
        if (wptr) inBuf->Unlock();
        return false;
    }
    if (IsEqualGUID(subtype, MFVideoFormat_NV12)) {
        UINT w = cfgW_, h = cfgH_;
        for (UINT y = 0; y < h; y++)
            memcpy(wptr + (size_t)y * w, src + (size_t)y * (size_t)stride, w);
        const BYTE* uv = src + (size_t)stride * h;
        BYTE* duv = wptr + (size_t)w * h;
        for (UINT y = 0; y < h / 2; y++)
            memcpy(duv + (size_t)y * w, uv + (size_t)y * (size_t)stride, w);
    } else {
        CopyRowsTopDown(wptr, packedStride, src, stride, (UINT)packedStride, cfgH_);
    }
    inBuf->Unlock();
    inBuf->SetCurrentLength(inSize);

    ATL::CComPtr<IMFSample> inSample;
    if (FAILED(MFCreateSample(&inSample)) || !inSample)
        return false;
    if (FAILED(inSample->AddBuffer(inBuf)))
        return false;
    inBuf = nullptr;
    // Монотонные таймстампы 30 FPS: одинаковый ts=0 на всех сэмплах путает MFT.
    const LONGLONG ts = frameIndex_++ * kFrameDuration;
    inSample->SetSampleTime(ts);
    inSample->SetSampleDuration(kFrameDuration);

    HRESULT hr = mft->ProcessInput(0, inSample, 0);
    inSample = nullptr;
    if (hr == MF_E_NOTACCEPTING || hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
        rejected = true; // caller: FLUSH + один ретрай; сейчас — в fallback
        LogMft(L"ProcessInput rejected, will flush+retry: " + HrHex(hr));
        return false;
    }
    if (FAILED(hr)) {
        LogMft(L"ProcessInput failed: " + HrHex(hr));
        return false;
    }

    ATL::CComPtr<IMFMediaBuffer> outBuf;
    if (FAILED(MFCreateMemoryBuffer(outBufSize_, &outBuf)) || !outBuf)
        return false;
    ATL::CComPtr<IMFSample> outSample;
    if (FAILED(MFCreateSample(&outSample)) || !outSample)
        return false;
    if (FAILED(outSample->AddBuffer(outBuf)))
        return false;
    outBuf = nullptr;

    MFT_OUTPUT_DATA_BUFFER ob = {};
    ob.dwStreamID = 0;
    ob.pSample = outSample;
    ob.dwStatus = 0;
    ob.pEvents = nullptr;
    DWORD status = 0;
    hr = mft->ProcessOutput(0, 1, &ob, &status);
    // ob.pSample — как правило тот же outSample (MFT не забирает владения:
    // ссылка caller'а жива в outSample до конца скоупа). Release здесь был бы
    // double-free -> порча кучи и падение после возврата. Чужой объект (если
    // MFT вернул свой) забираем через Attach, свой — шарим через AddRef.
    ATL::CComPtr<IMFSample> result;
    if (ob.pSample && ob.pSample != (IMFSample*)outSample)
        result.Attach(ob.pSample);
    else if (outSample)
        result = outSample;
    if (FAILED(hr)) {
        LogMft(L"ProcessOutput failed: " + HrHex(hr));
        if (ob.pEvents) ob.pEvents->Release();
        return false;
    }
    if (ob.pEvents) ob.pEvents->Release();

    bool ok = false;
    if (result) {
        ATL::CComPtr<IMFMediaBuffer> cont;
        if (SUCCEEDED(result->ConvertToContiguousBuffer(&cont)) && cont) {
            BYTE* rptr = nullptr;
            DWORD maxL = 0, curL = 0;
            if (SUCCEEDED(cont->Lock(&rptr, &maxL, &curL)) && rptr) {
                LONG os = outStride_;
                size_t need = (os >= 0 ? (size_t)os * (kOutH - 1) + kOutStride
                                       : (size_t)(-os) * (kOutH - 1) + kOutStride);
                if ((size_t)curL >= need && curL >= (size_t)kOutStride * kOutH) {
                    for (UINT y = 0; y < kOutH; y++) {
                        const BYTE* srow = (os >= 0)
                            ? rptr + (size_t)y * (size_t)os
                            : rptr + (size_t)(kOutH - 1 - y) * (size_t)(-os);
                        memcpy(dst + (size_t)y * (size_t)kOutStride, srow,
                               (size_t)kOutStride);
                    }
                    ok = true;
                } else {
                    LogMft(L"output too small: cur=" + std::to_wstring(curL));
                }
                cont->Unlock();
            }
            cont = nullptr;
        }
    }
    // Stateless 1:1 — сбросить pending сразу после успеха (см. Process).
    if (ok && streaming_)
        mft->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
    return ok;
}
