#include "V2FrameReader.h"
#include <wchar.h>

namespace vcam_v2 {
namespace {

constexpr int kPrefixCount = 2;
const wchar_t* const kPrefixes[kPrefixCount] = { L"Global\\", L"Local\\" };

const wchar_t* BaseName(const wchar_t* namedObject)
{
    const wchar_t* pSep = wcschr(namedObject, L'\\');
    return (pSep != nullptr) ? pSep + 1 : namedObject;
}

} // namespace

V2Reader::V2Reader() = default;

V2Reader::~V2Reader()
{
    Close();
}

void V2Reader::Close()
{
    m_pHeader = nullptr;
    m_cbMapped = 0;
    m_view.Close();
    m_hReadyEvent.Close();
    m_hSection.Close();
}

bool V2Reader::EnsureOpen()
{
    ATL::CComCritSecLock<ATL::CComAutoCriticalSection> guard(m_cs);
    if (m_pHeader != nullptr) return true;

    // ТОЛЬКО чтение существующей секции: OpenFileMapping(FILE_MAP_READ),
    // никакого CreateFileMapping (писателя не открываем, секцию не создаём).
    for (int prefix = 0; prefix < kPrefixCount && m_hSection == nullptr; ++prefix) {
        wchar_t name[MAX_PATH] = {};
        swprintf_s(name, ARRAYSIZE(name), L"%s%s", kPrefixes[prefix],
                   BaseName(vcam::VCamSectionNameV2));
        m_hSection.Attach(OpenFileMappingW(FILE_MAP_READ, FALSE, name));
    }
    if (m_hSection == nullptr) return false;

    // Событие — только ждать (SYNCHRONIZE): ResetEvent не зовём (нет прав и не
    // надо — auto-reset само сбрасывается на Wait). Нет события — seq-poll.
    for (int prefix = 0; prefix < kPrefixCount && m_hReadyEvent == nullptr; ++prefix) {
        wchar_t name[MAX_PATH] = {};
        swprintf_s(name, ARRAYSIZE(name), L"%s%s", kPrefixes[prefix],
                   BaseName(vcam::VCamReadyEventNameV2));
        m_hReadyEvent.Attach(OpenEventW(SYNCHRONIZE, FALSE, name));
    }

    void* pRaw = MapViewOfFile(m_hSection, FILE_MAP_READ, 0, 0, 0);
    if (!m_view.Attach(pRaw)) {
        Close();
        return false;
    }
    MEMORY_BASIC_INFORMATION mbi = {};
    if (VirtualQuery(m_view.Get(), &mbi, sizeof(mbi)) != 0)
        m_cbMapped = mbi.RegionSize;

    const vcam::VCamSectionHeader* h = m_view.GetAs<vcam::VCamSectionHeader>();
    if (m_cbMapped < sizeof(vcam::VCamSectionHeader) || !IsV2HeaderValid(h, m_cbMapped)) {
        Close(); // мусор/чужой — не держим, фолбэк на v1 у вызывающего
        return false;
    }
    m_pHeader = h;
    return true;
}

bool V2Reader::ReadSnapshot(UINT32* pW, UINT32* pH, UINT32* pStride,
                            UINT32* pFrameSize, UINT32* pIdx)
{
    // Вызывать при открытом m_pHeader; seqlock-триал без ожидания.
    for (int spin = 0; spin < 64; ++spin) {
        const LONGLONG seq = m_pHeader->seq;
        if (seq & 1) { YieldProcessor(); continue; }
        const UINT32 w = m_pHeader->width;
        const UINT32 h = m_pHeader->height;
        const UINT32 stride = m_pHeader->stride;
        const UINT32 frameSize = m_pHeader->frameSize;
        const UINT32 idx = m_pHeader->frameWriteIndex;
        const UINT32 slots = m_pHeader->slotCount;
        if (idx >= slots) return false;
        if (m_pHeader->seq != seq) continue;
        // Проверяем связку целиком (диметры меняются только внутри seqlock).
        vcam::VCamSectionHeader tmp = *m_pHeader;
        tmp.width = w; tmp.height = h; tmp.stride = stride;
        tmp.frameSize = frameSize; tmp.slotCount = slots;
        if (!IsV2HeaderValid(&tmp, m_cbMapped)) return false;
        if (m_pHeader->seq != seq) continue;
        if (pW) *pW = w;
        if (pH) *pH = h;
        if (pStride) *pStride = stride;
        if (pFrameSize) *pFrameSize = frameSize;
        if (pIdx) *pIdx = idx;
        return true;
    }
    return false;
}

bool V2Reader::Probe(UINT32* pW, UINT32* pH, UINT32* pStride, UINT32* pFrameSize)
{
    if (!EnsureOpen()) return false;
    return ReadSnapshot(pW, pH, pStride, pFrameSize, nullptr);
}

bool V2Reader::Acquire(BYTE* pDest, SIZE_T cap, UINT32* pW, UINT32* pH,
                       UINT32* pStride, DWORD timeoutMs)
{
    if (pDest == nullptr) return false;
    if (!EnsureOpen()) return false;

    HANDLE hReady = nullptr;
    {
        ATL::CComCritSecLock<ATL::CComAutoCriticalSection> guard(m_cs);
        hReady = m_hReadyEvent.m_h;
        if (m_pHeader == nullptr) return false;
    }

    if (hReady != nullptr) {
        if (WaitForSingleObject(hReady, timeoutMs) != WAIT_OBJECT_0)
            return false; // таймаут — вызывающий идёт v1-путём (кэш/NO SIGNAL)
    } else {
        // События нет — ждём продвижения seq, как v1-читатель.
        const LONGLONG startSeq = m_pHeader->seq;
        const ULONGLONG deadline = GetTickCount64() + timeoutMs;
        while (m_pHeader->seq == startSeq && GetTickCount64() < deadline)
            Sleep(1);
        if (m_pHeader->seq == startSeq) return false;
    }

    for (int spin = 0; spin < 1000; ++spin) {
        const LONGLONG seq = m_pHeader->seq;
        if (seq & 1) { YieldProcessor(); continue; }
        UINT32 w = 0, h = 0, stride = 0, frameSize = 0, idx = 0;
        if (!ReadSnapshot(&w, &h, &stride, &frameSize, &idx)) return false;
        if ((SIZE_T)stride * h > cap) return false;
        const BYTE* pSrc = (const BYTE*)m_view.Get() +
                           sizeof(vcam::VCamSectionHeader) +
                           (SIZE_T)idx * vcam::VCamV2MaxFrameSize;
        for (UINT32 y = 0; y < h; ++y)
            memcpy(pDest + (SIZE_T)y * stride, pSrc + (SIZE_T)y * stride,
                   (SIZE_T)w * 4);
        if (m_pHeader->seq != seq) continue; // гонка — перечитать
        if (pW) *pW = w;
        if (pH) *pH = h;
        if (pStride) *pStride = stride;
        return true;
    }
    return false;
}

} // namespace vcam_v2
