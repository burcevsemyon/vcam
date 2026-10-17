#pragma once

#include "CaptureTestPCH.h"

struct Stream0Info {
    bool haveImage = false;
    bool havePerFrameStats = false;
    MFRatio sar = { 1, 1 };
    RECT rcSource;
    RECT rcOutput;
    GUID sub;
    UINT32 dwWidth = 0;
    UINT32 dwHeight = 0;
    UINT32 lVideoSampleRate = 0;
    UINT32 dwDefaultStride = 0;
};

struct FrameCtx {
    int index = 0;
    DWORD curLen = 0;
    ULONGLONG requestMs = 0;
    ULONGLONG tMs = 0;
    const BYTE* pBits = nullptr;
    Stream0Info s0{};
};

using FrameCallback = void (*)(void* ctx, const FrameCtx& fc);

struct CapOptions {
    const wchar_t* nameFilter = L"VCam";
    UINT32 reqW = 1280, reqH = 720;
    bool wantNv12 = false;
    int numFrames = 30;
    int intervalMs = 100;
    ULONGLONG deadlineMs = 60000;
};

struct ModeArgs {
    const wchar_t* nameFilter = L"VCam";
    UINT32 reqW = 1280, reqH = 720;
    int frames = 30;
    int intervalMs = 100;
    int durationSec = 60;
    double threshold = 40.0;
    int allowBlack = 0;
    const wchar_t* prefix = L"cap";
    const wchar_t* logFile = nullptr;
    const wchar_t* settingsFile = nullptr;
    bool json = false;
    bool nv12 = false;
    bool saveBlack = false;
};

struct PixelStats {
    double mean = 0;
    double stddev = 0;
    int min = 255;
    int max = 0;
};

struct FrameRec {
    int index = 0;
    ULONGLONG tMs = 0;
    ULONGLONG requestMs = 0;
    DWORD curLen = 0;
    PixelStats px;
    wchar_t hash[65] = {};
    bool black = false;
    Stream0Info s0{};
    const BYTE* pBits = nullptr;
};

struct MfSession {
    bool comOk = false;
    bool mfOk = false;
    ~MfSession()
    {
        if (mfOk) MFShutdown();
        if (comOk) CoUninitialize();
    }
};

struct CToken : public IUnknown
{
    ULONG m_ref = 1;
    HRESULT QueryInterface(REFIID riid, void** ppv) override
    {
        if (riid == IID_IUnknown) { *ppv = static_cast<IUnknown*>(this); AddRef(); return S_OK; }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG AddRef() override { return ++m_ref; }
    ULONG Release() override { ULONG r = --m_ref; if (r == 0) delete this; return r; }
};