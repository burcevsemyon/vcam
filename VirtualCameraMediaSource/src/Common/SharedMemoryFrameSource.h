#pragma once
#include <windows.h>
#include <mfobjects.h>
#include <memory>
#include "SharedMemoryContract.h"
#include "MappedViewOfFilePtr.h"

class SharedMemoryFrameSource {
public:
    static SharedMemoryFrameSource& Instance();

    HRESULT Init();
    // Copy latest frame into pDest (must be VCamFrameSize bytes).
    // On timeout: use last cached frame or fill black. Always returns S_OK on success.
    HRESULT AcquireFrame(BYTE* pDest, DWORD timeoutMs);
    // Heartbeat потребителя: сессия стартовала (первый сэмпл может прийти
    // позже). Пишет readerLastActiveTick, пока сессия живёт.
    void TouchReader();
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
    vcam::MappedViewOfFilePtr m_view;
    vcam::VCamSectionHeader* m_pHeader = nullptr;
    std::unique_ptr<BYTE[]> m_pCache; // owned
    bool m_bHaveCache = false;
    ULONGLONG m_lastFreshMs = 0; // GetTickCount64 of last fresh frame read
    bool m_bOffline = false;
};
