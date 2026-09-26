#include "SharedMemoryFrameSource.h"
#include <new>
#include <sddl.h>
#include <wchar.h>

namespace {

constexpr int kNamePrefixCount = 2;
const wchar_t* const kNamePrefixes[kNamePrefixCount] = { L"Global\\", L"Local\\" };

const wchar_t* ObjectBaseName(const wchar_t* namedObjectName)
{
    const wchar_t* pSep = wcschr(namedObjectName, L'\\');
    return (pSep != nullptr) ? pSep + 1 : namedObjectName;
}

bool IsRetryableOpenError(DWORD win32Error)
{
    return win32Error == ERROR_FILE_NOT_FOUND || win32Error == ERROR_ACCESS_DENIED;
}

struct CsGuard {
    CRITICAL_SECTION* cs;
    explicit CsGuard(CRITICAL_SECTION* c) : cs(c) { EnterCriticalSection(c); }
    ~CsGuard() { LeaveCriticalSection(cs); }
};

static void PaintNoSignalPattern(BYTE* pDest, UINT32 width, UINT32 height, UINT32 stride)
{
    for (UINT32 y = 0; y < height; ++y) {
        for (UINT32 x = 0; x < width; ++x) {
            BYTE* pPixel = pDest + (SIZE_T)y * stride + (SIZE_T)x * 4;
            bool isBorder = (x < 6 || x >= width - 6 || y < 6 || y >= height - 6);
            bool isGrid = ((x % 160 == 0) || (y % 160 == 0) || (x == width / 2) || (y == height / 2));

            if (isBorder) {
                pPixel[0] = 50;  pPixel[1] = 120; pPixel[2] = 220; pPixel[3] = 0xFF; // Orange/Amber border
            } else if (isGrid) {
                pPixel[0] = 200; pPixel[1] = 200; pPixel[2] = 200; pPixel[3] = 0xFF; // White/Gray grid lines
            } else {
                pPixel[0] = 60;  pPixel[1] = 30;  pPixel[2] = 20;  pPixel[3] = 0xFF; // Dark blue/slate background
            }
        }
    }
}

} // namespace

SharedMemoryFrameSource& SharedMemoryFrameSource::Instance()
{
    static SharedMemoryFrameSource s_instance;
    return s_instance;
}

SharedMemoryFrameSource::~SharedMemoryFrameSource()
{
    Shutdown();
}

HRESULT SharedMemoryFrameSource::Init()
{
    if (m_bInit) return S_OK;
    if (m_bShutDown) return E_UNEXPECTED;

    InitializeCriticalSection(&m_cs);

    PSECURITY_DESCRIPTOR pSecDesc = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
        vcam::VCamDacSddl, SDDL_REVISION_1, &pSecDesc, nullptr)) {
        m_bShutDown = true;
        return HRESULT_FROM_WIN32(GetLastError());
    }

    SECURITY_ATTRIBUTES sa = {};
    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = pSecDesc;
    sa.bInheritHandle = FALSE;

    SIZE_T totalSize = sizeof(vcam::VCamSectionHeader) + (SIZE_T)vcam::VCamSlotCount * vcam::VCamFrameSize;

    for (int prefix = 0; prefix < kNamePrefixCount && m_hSection == nullptr; ++prefix) {
        wchar_t sectionName[MAX_PATH] = {};
        swprintf_s(sectionName, ARRAYSIZE(sectionName), L"%s%s", kNamePrefixes[prefix], ObjectBaseName(vcam::VCamSectionName));
        m_hSection = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, sectionName);
        if (m_hSection == nullptr && !IsRetryableOpenError(GetLastError())) break;
    }
    for (int prefix = 0; prefix < kNamePrefixCount && m_hSection == nullptr; ++prefix) {
        wchar_t sectionName[MAX_PATH] = {};
        swprintf_s(sectionName, ARRAYSIZE(sectionName), L"%s%s", kNamePrefixes[prefix], ObjectBaseName(vcam::VCamSectionName));
        m_hSection = CreateFileMappingW(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE,
            (DWORD)(totalSize >> 32), (DWORD)(totalSize & 0xFFFFFFFF), sectionName);
        if (m_hSection == nullptr && GetLastError() != ERROR_ACCESS_DENIED) break;
    }
    for (int prefix = 0; prefix < kNamePrefixCount && m_hReadyEvent == nullptr; ++prefix) {
        wchar_t eventName[MAX_PATH] = {};
        swprintf_s(eventName, ARRAYSIZE(eventName), L"%s%s", kNamePrefixes[prefix], ObjectBaseName(vcam::VCamReadyEventName));
        m_hReadyEvent = OpenEventW(EVENT_MODIFY_STATE | SYNCHRONIZE, FALSE, eventName);
    }
    if (m_hSection == nullptr) {
        if (m_hReadyEvent) { CloseHandle(m_hReadyEvent); m_hReadyEvent = nullptr; }
        if (m_hSection) { CloseHandle(m_hSection); m_hSection = nullptr; }
        if (pSecDesc) LocalFree(pSecDesc);

        m_pCache = new (std::nothrow) BYTE[vcam::VCamFrameSize];
        if (m_pCache == nullptr) {
            m_bShutDown = true;
            return E_OUTOFMEMORY;
        }
        m_bOffline = true;
        m_bInit = true;
        return S_OK;
    }

    m_pBase = (BYTE*)MapViewOfFileEx(m_hSection, FILE_MAP_ALL_ACCESS, 0, 0, totalSize, nullptr);
    if (m_pBase == nullptr) {
        if (m_pBase) { UnmapViewOfFile(m_pBase); m_pBase = nullptr; m_pHeader = nullptr; }
        if (m_hReadyEvent) { CloseHandle(m_hReadyEvent); m_hReadyEvent = nullptr; }
        if (m_hSection) { CloseHandle(m_hSection); m_hSection = nullptr; }
        if (pSecDesc) LocalFree(pSecDesc);

        m_pCache = new (std::nothrow) BYTE[vcam::VCamFrameSize];
        if (m_pCache == nullptr) {
            m_bShutDown = true;
            return E_OUTOFMEMORY;
        }
        m_bOffline = true;
        m_bInit = true;
        return S_OK;
    }

    if (pSecDesc) LocalFree(pSecDesc);

    m_pHeader = reinterpret_cast<vcam::VCamSectionHeader*>(m_pBase);
    if (m_pHeader->magic != vcam::VCamMagic) {
        m_pHeader->magic = vcam::VCamMagic;
        m_pHeader->version = vcam::VCamVersion;
        m_pHeader->width = vcam::VCamWidth;
        m_pHeader->height = vcam::VCamHeight;
        m_pHeader->stride = vcam::VCamStride;
        m_pHeader->pixelFormat = (UINT32)vcam::VCamPixelFormat::RGB32;
        m_pHeader->frameSize = vcam::VCamFrameSize;
        m_pHeader->slotCount = vcam::VCamSlotCount;
        m_pHeader->frameWriteIndex = 0;
        m_pHeader->seq = 0;
        m_pHeader->lastFrameTime100ns = 0;
    }
    m_pCache = new (std::nothrow) BYTE[vcam::VCamFrameSize];
    if (m_pCache == nullptr) {
        UnmapViewOfFile(m_pBase); m_pBase = nullptr; m_pHeader = nullptr;
        CloseHandle(m_hReadyEvent); m_hReadyEvent = nullptr;
        CloseHandle(m_hSection); m_hSection = nullptr;
        m_bShutDown = true;
        return E_OUTOFMEMORY;
    }

    m_bInit = true;
    return S_OK;
}

HRESULT SharedMemoryFrameSource::AcquireFrame(BYTE* pDest, DWORD timeoutMs)
{
    if (!m_bInit || m_bShutDown) return E_UNEXPECTED;
    if (pDest == nullptr) return E_POINTER;
    CsGuard guard(&m_cs); // сериализация параллельных клиентов (кэш + событие)

    if (m_bOffline) {
        Sleep(33);
        return FallbackFrame(pDest);
    }

    if (m_hReadyEvent != nullptr) {
        if (WaitForSingleObject(m_hReadyEvent, timeoutMs) != WAIT_OBJECT_0) {
            return FallbackFrame(pDest);
        }
        ResetEvent(m_hReadyEvent);
    } else {
        // Событие недоступно — ждём продвижения seq (poll) в пределах таймаута.
        LONGLONG startSeq = m_pHeader->seq;
        ULONGLONG deadline = GetTickCount64() + timeoutMs;
        while (m_pHeader->seq == startSeq && GetTickCount64() < deadline) {
            Sleep(1);
        }
        if (m_pHeader->seq == startSeq) {
            return FallbackFrame(pDest);
        }
    }
    for (int spin = 0; ; ++spin) {
        LONGLONG seq = m_pHeader->seq;
        if (seq & 1) {
            if (spin < 1000) { YieldProcessor(); continue; }
            return FallbackFrame(pDest);
        }
        UINT32 idx = m_pHeader->frameWriteIndex;
        if (idx >= m_pHeader->slotCount) {
            return FallbackFrame(pDest);
        }
        LONGLONG seq2 = m_pHeader->seq;
        if (seq != seq2) continue;

        const BYTE* pSrc = m_pBase + sizeof(vcam::VCamSectionHeader) + (SIZE_T)idx * vcam::VCamFrameSize;
        const DWORD rowBytes = (DWORD)(vcam::VCamWidth * vcam::VCamPixelSize);
        for (UINT32 y = 0; y < vcam::VCamHeight; ++y) {
            memcpy(pDest + (SIZE_T)y * vcam::VCamStride, pSrc + (SIZE_T)y * vcam::VCamStride, rowBytes);
        }
        if (m_pCache) {
            memcpy(m_pCache, pDest, vcam::VCamFrameSize);
            m_bHaveCache = true;
        }
        return S_OK;
    }
}

HRESULT SharedMemoryFrameSource::FallbackFrame(BYTE* pDest)
{
    if (m_pCache && m_bHaveCache) {
        memcpy(pDest, m_pCache, vcam::VCamFrameSize);
        return S_OK;
    }
    PaintNoSignalPattern(pDest, vcam::VCamWidth, vcam::VCamHeight, vcam::VCamStride);
    return S_OK;
}

void SharedMemoryFrameSource::Shutdown()
{
    EnterCriticalSection(&m_cs);
    if (m_bShutDown) { LeaveCriticalSection(&m_cs); return; }
    m_bShutDown = true;
    m_bInit = false;
    if (m_pBase) { UnmapViewOfFile(m_pBase); m_pBase = nullptr; m_pHeader = nullptr; }
    if (m_pCache) { delete[] m_pCache; m_pCache = nullptr; }
    if (m_hReadyEvent) { CloseHandle(m_hReadyEvent); m_hReadyEvent = nullptr; }
    if (m_hSection) { CloseHandle(m_hSection); m_hSection = nullptr; }
    LeaveCriticalSection(&m_cs);
}
