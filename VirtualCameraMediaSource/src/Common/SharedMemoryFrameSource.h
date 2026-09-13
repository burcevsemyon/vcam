#pragma once
#include <windows.h>
#include <mfobjects.h>
#include "SharedMemoryContract.h"

class SharedMemoryFrameSource {
public:
    static SharedMemoryFrameSource& Instance();

    HRESULT Init();
    // Copy latest frame into pDest (must be VCamFrameSize bytes).
    // On timeout: use last cached frame or fill black. Always returns S_OK on success.
    HRESULT AcquireFrame(BYTE* pDest, DWORD timeoutMs);
    void Shutdown();

    SharedMemoryFrameSource(const SharedMemoryFrameSource&) = delete;
    SharedMemoryFrameSource& operator=(const SharedMemoryFrameSource&) = delete;

private:
    SharedMemoryFrameSource() = default;
    ~SharedMemoryFrameSource();
    HRESULT FallbackFrame(BYTE* pDest);

    CRITICAL_SECTION m_cs;
    bool m_bInit = false;
    bool m_bShutDown = false;
    HANDLE m_hSection = nullptr;
    HANDLE m_hReadyEvent = nullptr;
    BYTE* m_pBase = nullptr;
    vcam::VCamSectionHeader* m_pHeader = nullptr;
    BYTE* m_pCache = nullptr;
    bool m_bHaveCache = false;
};
