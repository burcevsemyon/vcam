#include "SharedMemoryFrameSource.h"
#include <new>
#include <vector>
#include <sddl.h>
#include <wchar.h>

#include "FrameCopy.h"
#include "SectionHeaderInit.h"

namespace {

constexpr int kNamePrefixCount = 2;
const wchar_t* const kNamePrefixes[kNamePrefixCount] = { L"Global\\", L"Local\\" };

// Кэш последнего кадра отдаётся только в течение этого окна после последнего
// свежего кадра; дальше — NO SIGNAL. Перекрывает hot-switch окно хоста (5000 мс).
constexpr ULONGLONG kNoSignalAfterMs = 7000;

const wchar_t* ObjectBaseName(const wchar_t* namedObjectName)
{
    const wchar_t* pSep = wcschr(namedObjectName, L'\\');
    return (pSep != nullptr) ? pSep + 1 : namedObjectName;
}

bool IsRetryableOpenError(DWORD win32Error)
{
    return win32Error == ERROR_FILE_NOT_FOUND || win32Error == ERROR_ACCESS_DENIED;
}

// Паттерн не зависит от кадра — рисуем один раз на размер и дальше копируем.
// Вызывается только из FallbackFrame под m_cs, статический кэш безопасен.
static void PaintNoSignalPattern(BYTE* pDest, UINT32 width, UINT32 height, UINT32 stride)
{
    static std::vector<BYTE> s_cache;
    static UINT32 s_w = 0, s_h = 0, s_stride = 0;
    const size_t bytes = (size_t)stride * height;
    if (s_cache.size() != bytes || s_w != width || s_h != height || s_stride != stride) {
        s_cache.assign(bytes, 0);
        BYTE* pBits = s_cache.data();
        for (UINT32 y = 0; y < height; ++y) {
            for (UINT32 x = 0; x < width; ++x) {
                BYTE* pPixel = pBits + (SIZE_T)y * stride + (SIZE_T)x * 4;
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
        s_w = width; s_h = height; s_stride = stride;
    }
    memcpy(pDest, s_cache.data(), bytes);
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
        m_hSection.Attach(OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, sectionName));
        if (m_hSection == nullptr && !IsRetryableOpenError(GetLastError())) break;
    }
    for (int prefix = 0; prefix < kNamePrefixCount && m_hSection == nullptr; ++prefix) {
        wchar_t sectionName[MAX_PATH] = {};
        swprintf_s(sectionName, ARRAYSIZE(sectionName), L"%s%s", kNamePrefixes[prefix], ObjectBaseName(vcam::VCamSectionName));
        m_hSection.Attach(CreateFileMappingW(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE,
            (DWORD)(totalSize >> 32), (DWORD)(totalSize & 0xFFFFFFFF), sectionName));
        if (m_hSection == nullptr && GetLastError() != ERROR_ACCESS_DENIED) break;
    }
    for (int prefix = 0; prefix < kNamePrefixCount && m_hReadyEvent == nullptr; ++prefix) {
        wchar_t eventName[MAX_PATH] = {};
        swprintf_s(eventName, ARRAYSIZE(eventName), L"%s%s", kNamePrefixes[prefix], ObjectBaseName(vcam::VCamReadyEventName));
        m_hReadyEvent.Attach(OpenEventW(EVENT_MODIFY_STATE | SYNCHRONIZE, FALSE, eventName));
    }
    if (m_hSection == nullptr) {
        m_hReadyEvent.Close();
        if (pSecDesc) LocalFree(pSecDesc);

        try { m_pCache = std::make_unique_for_overwrite<BYTE[]>(vcam::VCamFrameSize); }
        catch (const std::bad_alloc&) { m_pCache.reset(); }
        if (m_pCache == nullptr) {
            m_bShutDown = true;
            return E_OUTOFMEMORY;
        }
        m_bOffline = true;
        m_bInit = true;
        return S_OK;
    }

    m_view.Attach(MapViewOfFileEx(m_hSection, FILE_MAP_ALL_ACCESS, 0, 0, totalSize, nullptr));
    if (!m_view) {
        m_hReadyEvent.Close();
        m_hSection.Close();
        if (pSecDesc) LocalFree(pSecDesc);

        try { m_pCache = std::make_unique_for_overwrite<BYTE[]>(vcam::VCamFrameSize); }
        catch (const std::bad_alloc&) { m_pCache.reset(); }
        if (m_pCache == nullptr) {
            m_bShutDown = true;
            return E_OUTOFMEMORY;
        }
        m_bOffline = true;
        m_bInit = true;
        return S_OK;
    }

    if (pSecDesc) LocalFree(pSecDesc);

    m_pHeader = m_view.GetAs<vcam::VCamSectionHeader>();
    if (m_pHeader->magic != vcam::VCamMagic) {
        vcam::InitSectionHeader(m_pHeader);
    }
    try { m_pCache = std::make_unique_for_overwrite<BYTE[]>(vcam::VCamFrameSize); }
    catch (const std::bad_alloc&) { m_pCache.reset(); }
    if (m_pCache == nullptr) {
        m_view.Close(); m_pHeader = nullptr;
        m_hReadyEvent.Close();
        m_hSection.Close();
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
    // Heartbeat потребителя: вызов идёт только когда живая MF-сессия тянет
    // сэмплы (включая fallback/NO SIGNAL) — хост/hold-watch видят «потребитель есть».
    TouchReader();

    // Фаза 1 (под локом): снимок режима/события/заголовка. Гард защищает
    // только reader-состояние (кэш/хендлы/флаги/вью секции); seqlock-чтение
    // кадра синхронизируется с писателем через seq в SHM (другой процесс),
    // не через этот лок. Wait/sleep — ВНЕ лока: параллельные клиенты не
    // сериализуются на ожидании кадра (см. фазу 2).
    bool offline;
    HANDLE hReady;
    vcam::VCamSectionHeader* pHeader;
    LONGLONG startSeq = 0;
    {
        ATL::CComCritSecLock<ATL::CComAutoCriticalSection> guard(m_cs);
        if (!m_bInit || m_bShutDown) return E_UNEXPECTED;
        offline = m_bOffline;
        hReady = m_hReadyEvent;
        pHeader = m_pHeader;
        if (!offline && hReady == nullptr && pHeader != nullptr) {
            startSeq = pHeader->seq;
        }
    }

    // Фаза 2 (ВНЕ лока): ожидание свежего кадра. Событие FrameReady —
    // auto-reset (CreateEventW(..., FALSE, FALSE, ...) у всех продьюсеров):
    // отпускает ровно одного ожидающего на publish, паритет «один publish —
    // один пробуждший клиент» сохраняется и без удержания лока.
    bool waitOk = true;     // event-ветка: событие успело сработать
    bool progressed = true; // poll-ветка: seq продвинулся
    if (offline) {
        Sleep(33);
    } else if (hReady != nullptr) {
        waitOk = (WaitForSingleObject(hReady, timeoutMs) == WAIT_OBJECT_0);
    } else if (pHeader != nullptr) {
        // Событие недоступно — ждём продвижения seq (poll) в пределах таймаута.
        ULONGLONG deadline = GetTickCount64() + timeoutMs;
        while (pHeader->seq == startSeq && GetTickCount64() < deadline) {
            Sleep(1);
        }
        progressed = (pHeader->seq != startSeq);
    }

    // Фаза 3 (под локом): кэш/маппинг/копирование — как и раньше, но уже без
    // удержания лока во время ожидания.
    ATL::CComCritSecLock<ATL::CComAutoCriticalSection> guard(m_cs);
    if (!m_bInit || m_bShutDown) return FallbackFrame(pDest);
    if (offline) {
        return FallbackFrame(pDest);
    }
    if (m_pHeader == nullptr || !m_view) {
        return FallbackFrame(pDest);
    }
    if (hReady != nullptr) {
        if (!waitOk) return FallbackFrame(pDest);
        ResetEvent(hReady);
    } else if (!progressed) {
        return FallbackFrame(pDest);
    }
    // Seqlock: читаем seq до и после копирования кадра. Если seq нечётный —
    // писатель в момент чтения обновляет данные, повторяем. Если seq изменился
    // между двумя чтениями — кадр может быть смесью двух кадров, повторяем.
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

        const BYTE* pSrc = (const BYTE*)m_view.Get() + sizeof(vcam::VCamSectionHeader) + (SIZE_T)idx * vcam::VCamFrameSize;
        vcam::CopyFrameRowwise(pDest, vcam::VCamStride, pSrc, vcam::VCamStride,
                               vcam::VCamWidth, vcam::VCamHeight, vcam::VCamPixelSize);
        if (m_pCache) {
            memcpy(m_pCache.get(), pDest, vcam::VCamFrameSize);
            m_bHaveCache = true;
        }
        m_lastFreshMs = GetTickCount64();
        return S_OK;
    }
}

void SharedMemoryFrameSource::TouchReader()
{
    if (!m_bInit || m_bShutDown || m_bOffline || m_pHeader == nullptr) return;
    m_pHeader->readerLastActiveTick = GetTickCount64();
}

HRESULT SharedMemoryFrameSource::FallbackFrame(BYTE* pDest)
{
    // Кэш живёт только kNoSignalAfterMs после последнего свежего кадра: пока
    // писатель в hot-switch/микро-обрыве — статика; писатель мёртв — NO SIGNAL.
    if (m_pCache && m_bHaveCache && (GetTickCount64() - m_lastFreshMs) <= kNoSignalAfterMs) {
        memcpy(pDest, m_pCache.get(), vcam::VCamFrameSize);
        return S_OK;
    }
    PaintNoSignalPattern(pDest, vcam::VCamWidth, vcam::VCamHeight, vcam::VCamStride);
    return S_OK;
}

void SharedMemoryFrameSource::Shutdown()
{
    ATL::CComCritSecLock<ATL::CComAutoCriticalSection> guard(m_cs);
    if (m_bShutDown) { return; }
    m_bShutDown = true;
    m_bInit = false;
    m_view.Close(); m_pHeader = nullptr;
    m_pCache.reset();
    m_hReadyEvent.Close();
    m_hSection.Close();
}
