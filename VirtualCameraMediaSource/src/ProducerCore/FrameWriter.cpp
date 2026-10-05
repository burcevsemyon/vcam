#include "FrameWriter.h"

#include <sddl.h>

#include "FrameCopy.h"
#include "ImageLayout.h"
#include "SectionHeaderInit.h"

#pragma comment(lib, "advapi32.lib")

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002 // Win10 1803+
#endif

namespace {

void LogWriter(const std::wstring& msg)
{
    OutputDebugStringW((L"[ProducerCore] " + msg + L"\n").c_str());
}

constexpr LONGLONG kFrameIntervalMs = 33; // 30 FPS

} // namespace

FrameWriter::FrameWriter()
{
    QueryPerformanceFrequency(&freq_);
    cache_.resize(vcam::VCamFrameSize);
}

FrameWriter::~FrameWriter()
{
    Close();
    if (hPaceTimer_) { CloseHandle(hPaceTimer_); hPaceTimer_ = nullptr; }
}

bool FrameWriter::Open(std::wstring& err)
{
    if (open_) return true;

    if (FAILED(ConvertStringSecurityDescriptorToSecurityDescriptorW(
            vcam::VCamDacSddl, SDDL_REVISION_1, &pSecDesc_, nullptr))) {
        err = L"ConvertStringSecurityDescriptorToSecurityDescriptorW failed";
        pSecDesc_ = nullptr;
        return false;
    }
    SECURITY_ATTRIBUTES sa = { sizeof(sa), pSecDesc_, FALSE };

    const wchar_t* pSep = wcschr(vcam::VCamSectionName, L'\\');
    const std::wstring baseName = (pSep != nullptr) ? pSep + 1 : vcam::VCamSectionName;
    openedSection_.clear();

    SIZE_T totalSize = sizeof(vcam::VCamSectionHeader) +
                       (SIZE_T)vcam::VCamSlotCount * vcam::VCamFrameSize;
    hSection_.Attach(CreateFileMappingW(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE,
        (DWORD)(totalSize >> 32), (DWORD)(totalSize & 0xFFFFFFFF), vcam::VCamSectionName));
    if (hSection_) {
        openedSection_ = vcam::VCamSectionName;
    } else {
        DWORD createErr = GetLastError();
        // Limited-токен (автозапуск из HKCU Run) без SeCreateGlobalPrivilege не может
        // СОЗДАВАТЬ Global\-объекты (ERROR_ACCESS_DENIED). Открыть уже существующую
        // секцию и создать сеансовую Local\-копию при этом можно — читатели
        // (SharedMemoryFrameSource) перебирают Global\ -> Local\ сами.
        hSection_.Attach(OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, vcam::VCamSectionName));
        if (hSection_) {
            openedSection_ = vcam::VCamSectionName;
            LogWriter(L"section: opened existing Global after create failed: " +
                      std::to_wstring(createErr));
        } else {
            const std::wstring localName = L"Local\\" + baseName;
            hSection_.Attach(CreateFileMappingW(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE,
                (DWORD)(totalSize >> 32), (DWORD)(totalSize & 0xFFFFFFFF), localName.c_str()));
            if (hSection_) {
                openedSection_ = localName;
                LogWriter(L"section: created Local fallback (Global create failed: " +
                          std::to_wstring(createErr) + L")");
            }
        }
    }
    if (!hSection_) {
        err = L"CreateFileMappingW failed: " + std::to_wstring(GetLastError());
        LocalFree(pSecDesc_);
        pSecDesc_ = nullptr;
        return false;
    }

    hReady_.Attach(CreateEventW(&sa, FALSE, FALSE, vcam::VCamReadyEventName));
    if (!hReady_) {
        // Нет SeCreateGlobalPrivilege — пробуем открыть уже существующее событие
        // (созданное держателем/читателем); без него читатель переходит в poll-режим.
        DWORD createErr = GetLastError();
        hReady_.Attach(OpenEventW(EVENT_MODIFY_STATE | SYNCHRONIZE, FALSE, vcam::VCamReadyEventName));
        if (!hReady_ && openedSection_ != vcam::VCamSectionName) {
            // Секция в Local\ — событие тоже создаём в Local\ (читатели перебирают префиксы).
            const wchar_t* pSepE = wcschr(vcam::VCamReadyEventName, L'\\');
            const std::wstring baseEvent = (pSepE != nullptr) ? pSepE + 1 : vcam::VCamReadyEventName;
            const std::wstring localEvent = L"Local\\" + baseEvent;
            hReady_.Attach(CreateEventW(&sa, FALSE, FALSE, localEvent.c_str()));
        }
        if (!hReady_) {
            LogWriter(L"ready event unavailable: create=" + std::to_wstring(createErr) +
                      L" open=" + std::to_wstring(GetLastError()));
        }
    }
    view_.Attach(MapViewOfFileEx(hSection_, FILE_MAP_ALL_ACCESS, 0, 0, totalSize, nullptr));
    if (!view_) {
        err = L"MapViewOfFileEx failed: " + std::to_wstring(GetLastError());
        hReady_.Close();
        hSection_.Close();
        LocalFree(pSecDesc_); pSecDesc_ = nullptr;
        return false;
    }

    pHeader_ = view_.GetAs<vcam::VCamSectionHeader>();
    vcam::InitSectionHeader(pHeader_);

    // v2 — best effort: не открылась — живём на v1 (IsV2Open()=false).
    std::wstring v2err;
    if (!OpenV2(v2err, &sa)) {
        LogWriter(L"v2 section unavailable (v1-only mode): " + v2err);
    }

    LARGE_INTEGER start;
    QueryPerformanceCounter(&start);
    startCount_ = start.QuadPart;

    hasFrame_ = false;
    lastEmit_ = 0;
    open_ = true;
    return true;
}

void FrameWriter::Close()
{
    CloseV2();
    view_.Close();
    pHeader_ = nullptr;
    hReady_.Close();
    hSection_.Close();
    if (pSecDesc_) { LocalFree(pSecDesc_); pSecDesc_ = nullptr; }
    open_ = false;
    hasFrame_ = false;
}

bool FrameWriter::OpenV2(std::wstring& err, SECURITY_ATTRIBUTES* sa)
{
    const wchar_t* pSep = wcschr(vcam::VCamSectionNameV2, L'\\');
    const std::wstring baseName = (pSep != nullptr) ? pSep + 1 : vcam::VCamSectionNameV2;
    openedSectionV2_.clear();

    const SIZE_T totalSize = (SIZE_T)vcam::VCamV2TotalSize;
    hSectionV2_.Attach(CreateFileMappingW(INVALID_HANDLE_VALUE, sa, PAGE_READWRITE,
        (DWORD)(totalSize >> 32), (DWORD)(totalSize & 0xFFFFFFFF), vcam::VCamSectionNameV2));
    if (hSectionV2_) {
        openedSectionV2_ = vcam::VCamSectionNameV2;
    } else {
        DWORD createErr = GetLastError();
        hSectionV2_.Attach(OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, vcam::VCamSectionNameV2));
        if (hSectionV2_) {
            openedSectionV2_ = vcam::VCamSectionNameV2;
            LogWriter(L"v2 section: opened existing Global after create failed: " +
                      std::to_wstring(createErr));
        } else {
            const std::wstring localName = L"Local\\" + baseName;
            hSectionV2_.Attach(CreateFileMappingW(INVALID_HANDLE_VALUE, sa, PAGE_READWRITE,
                (DWORD)(totalSize >> 32), (DWORD)(totalSize & 0xFFFFFFFF), localName.c_str()));
            if (hSectionV2_) {
                openedSectionV2_ = localName;
                LogWriter(L"v2 section: created Local fallback (Global create failed: " +
                          std::to_wstring(createErr) + L")");
            }
        }
    }
    if (!hSectionV2_) {
        err = L"CreateFileMappingW(v2) failed: " + std::to_wstring(GetLastError());
        return false;
    }

    hReadyV2_.Attach(CreateEventW(sa, FALSE, FALSE, vcam::VCamReadyEventNameV2));
    if (!hReadyV2_) {
        DWORD createErr = GetLastError();
        hReadyV2_.Attach(OpenEventW(EVENT_MODIFY_STATE | SYNCHRONIZE, FALSE,
                                   vcam::VCamReadyEventNameV2));
        if (!hReadyV2_ && openedSectionV2_ != vcam::VCamSectionNameV2) {
            const wchar_t* pSepE = wcschr(vcam::VCamReadyEventNameV2, L'\\');
            const std::wstring baseEvent = (pSepE != nullptr) ? pSepE + 1 : vcam::VCamReadyEventNameV2;
            hReadyV2_.Attach(CreateEventW(sa, FALSE, FALSE,
                                          (L"Local\\" + baseEvent).c_str()));
        }
        if (!hReadyV2_) {
            LogWriter(L"v2 ready event unavailable: create=" + std::to_wstring(createErr) +
                      L" open=" + std::to_wstring(GetLastError()));
        }
    }
    viewV2_.Attach(MapViewOfFileEx(hSectionV2_, FILE_MAP_ALL_ACCESS, 0, 0, totalSize, nullptr));
    if (!viewV2_) {
        err = L"MapViewOfFileEx(v2) failed: " + std::to_wstring(GetLastError());
        hReadyV2_.Close();
        hSectionV2_.Close();
        return false;
    }

    pHeaderV2_ = viewV2_.GetAs<vcam::VCamSectionHeader>();
    // Стартовые диметры 720p — до первого нативного кадра читатель видит 720p.
    vcam::InitSectionHeaderV2(pHeaderV2_, vcam::VCamWidth, vcam::VCamHeight, vcam::VCamStride);

    try {
        cacheV2_.resize((size_t)vcam::VCamV2MaxFrameSize);
    } catch (...) {
        err = L"out of memory (v2 cache)";
        viewV2_.Close();
        pHeaderV2_ = nullptr;
        hReadyV2_.Close();
        hSectionV2_.Close();
        return false;
    }

    v2W_ = vcam::VCamWidth;
    v2H_ = vcam::VCamHeight;
    v2Stride_ = vcam::VCamStride;
    hasV2Frame_ = false;
    v2open_ = true;
    return true;
}

void FrameWriter::CloseV2()
{
    viewV2_.Close();
    pHeaderV2_ = nullptr;
    hReadyV2_.Close();
    hSectionV2_.Close();
    cacheV2_.clear();
    cacheV2_.shrink_to_fit();
    v2W_ = v2H_ = v2Stride_ = 0;
    hasV2Frame_ = false;
    v2open_ = false;
}

bool FrameWriter::PublishLocked()
{
    if (!open_ || !view_ || !pHeader_) return false;

    UINT32 slot = (pHeader_->frameWriteIndex + 1) % vcam::VCamSlotCount;
    uint8_t* dst = (uint8_t*)view_.Get() + sizeof(vcam::VCamSectionHeader) +
                   (SIZE_T)slot * vcam::VCamFrameSize;

    _InterlockedExchangeAdd64((volatile LONGLONG*)&pHeader_->seq, 1);
    memcpy(dst, cache_.data(), vcam::VCamFrameSize);
    pHeader_->frameWriteIndex = slot;
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    pHeader_->lastFrameTime100ns =
        (UINT64)((counter.QuadPart - startCount_) * 10000000 / freq_.QuadPart);
    _InterlockedExchangeAdd64((volatile LONGLONG*)&pHeader_->seq, 1);

    SetEvent(hReady_);
    return true;
}

const uint8_t* FrameWriter::V2SrcLocked() const
{
    // Зеркало 720p: cacheV2_ не заполняется (экономим полный memcpy на кадр),
    // в эфир идут те же байты, что уже лежат в cache_.
    if (v2W_ == vcam::VCamWidth && v2H_ == vcam::VCamHeight &&
        v2Stride_ == vcam::VCamStride)
        return cache_.data();
    return cacheV2_.data();
}

bool FrameWriter::PublishLockedV2()
{
    if (!v2open_ || !viewV2_ || !pHeaderV2_ || !hasV2Frame_) return false;
    if (v2W_ == 0 || v2H_ == 0 || v2Stride_ == 0 ||
        v2W_ > vcam::VCamV2MaxWidth || v2H_ > vcam::VCamV2MaxHeight ||
        v2Stride_ < v2W_ * 4 || v2Stride_ > vcam::VCamV2MaxStride) return false;

    const SIZE_T frameSize = (SIZE_T)v2H_ * v2Stride_;
    if (frameSize > cacheV2_.size()) return false;

    UINT32 slot = (pHeaderV2_->frameWriteIndex + 1) % vcam::VCamV2SlotCount;
    uint8_t* dst = (uint8_t*)viewV2_.Get() + sizeof(vcam::VCamSectionHeader) +
                   (SIZE_T)slot * vcam::VCamV2MaxFrameSize;

    _InterlockedExchangeAdd64((volatile LONGLONG*)&pHeaderV2_->seq, 1);
    // Диметры — внутри seqlock (читатель: seq1 -> dims+slot -> seq2).
    pHeaderV2_->width = v2W_;
    pHeaderV2_->height = v2H_;
    pHeaderV2_->stride = v2Stride_;
    pHeaderV2_->pixelFormat = (UINT32)vcam::VCamPixelFormat::RGB32;
    pHeaderV2_->frameSize = (UINT32)frameSize;
    pHeaderV2_->slotCount = vcam::VCamV2SlotCount;
    memcpy(dst, V2SrcLocked(), frameSize);
    pHeaderV2_->frameWriteIndex = slot;
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    pHeaderV2_->lastFrameTime100ns =
        (UINT64)((counter.QuadPart - startCount_) * 10000000 / freq_.QuadPart);
    _InterlockedExchangeAdd64((volatile LONGLONG*)&pHeaderV2_->seq, 1);

    SetEvent(hReadyV2_);
    return true;
}

void FrameWriter::Pace()
{
    if (freq_.QuadPart == 0) return;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    LONGLONG interval = freq_.QuadPart * kFrameIntervalMs / 1000;
    LONGLONG target = lastEmit_ + interval;
    if (target > now.QuadPart) {
        // Sleep() округляет до системного тика (~15.6 мс) и даёт джиттер кадров
        // 30 FPS. High-resolution waitable timer (Win10 1803+) ждёт точнее;
        // таймер создаётся лениво и переиспользуется, при отказе — прежний Sleep.
        bool waited = false;
        if (hPaceTimer_ == nullptr) {
            hPaceTimer_ = CreateWaitableTimerExW(nullptr, nullptr,
                CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        }
        if (hPaceTimer_ != nullptr) {
            // Отрицательный due time — относительное время, 100 нс.
            LARGE_INTEGER due;
            due.QuadPart = -((target - now.QuadPart) * 10000000 / freq_.QuadPart);
            if (due.QuadPart > -1) due.QuadPart = -1;
            if (SetWaitableTimer(hPaceTimer_, &due, 0, nullptr, nullptr, FALSE)) {
                WaitForSingleObject(hPaceTimer_, INFINITE);
                waited = true;
            }
        }
        if (!waited) {
            DWORD ms = (DWORD)((target - now.QuadPart) * 1000 / freq_.QuadPart);
            if (ms > 0) Sleep(ms);
        }
        lastEmit_ = target;
    } else {
        lastEmit_ = now.QuadPart;
    }
}

bool FrameWriter::WriteFrame(const uint8_t* bgrx, int stride)
{
    if (!open_) {
        LogWriter(L"WriteFrame: writer is not open");
        return false;
    }
    if (!bgrx || stride < (int)vcam::VCamStride) {
        LogWriter(L"WriteFrame: invalid buffer/stride");
        return false;
    }

    bool ok;
    {
        ATL::CComCritSecLock<ATL::CComAutoCriticalSection> guard(cs_);
        vcam::CopyFrameRowwise(cache_.data(), vcam::VCamStride, bgrx, (size_t)stride,
                               vcam::VCamWidth, vcam::VCamHeight, vcam::VCamPixelSize);
        hasFrame_ = true;
        ok = PublishLocked();
        // v2-зеркало 720p-входа (Sub3 переведёт хосты на WriteFrameNative).
        if (v2open_ && cacheV2_.size() >= vcam::VCamFrameSize) {
            // копия не нужна: V2SrcLocked при зеркале берёт cache_
            v2W_ = vcam::VCamWidth;
            v2H_ = vcam::VCamHeight;
            v2Stride_ = vcam::VCamStride;
            hasV2Frame_ = true;
            PublishLockedV2();
        }
    }

    if (!ok) {
        LogWriter(L"WriteFrame: publish failed");
        return false;
    }
    Pace();
    return true;
}

bool FrameWriter::WriteFrameNative(const uint8_t* bgrx, int stride, uint32_t w, uint32_t h)
{
    if (!open_) {
        LogWriter(L"WriteFrameNative: writer is not open");
        return false;
    }
    if (!bgrx || w == 0 || h == 0 || w > 8192 || h > 8192 ||
        stride < (int)(w * 4)) {
        LogWriter(L"WriteFrameNative: invalid buffer/dims");
        return false;
    }

    bool ok;
    {
        ATL::CComCritSecLock<ATL::CComAutoCriticalSection> guard(cs_);
    // v1: даунскейл натива до 720p letterbox (MFT, fallback CPU); вход уже
    // 720p-packed — прямое копирование как в WriteFrame.
    if (w == vcam::VCamWidth && h == vcam::VCamHeight && stride == (int)vcam::VCamStride) {
        vcam::CopyFrameRowwise(cache_.data(), vcam::VCamStride, bgrx, (size_t)stride,
                               vcam::VCamWidth, vcam::VCamHeight, vcam::VCamPixelSize);
    } else if (!mftScaler_.Scale(bgrx, w, h, (LONG)stride, cache_.data())) {
        vcam::LetterboxBilinearEx(bgrx, w, h, (LONG)stride, cache_.data(),
                                  vcam::VCamWidth, vcam::VCamHeight,
                                  (LONG)vcam::VCamStride);
    }
    hasFrame_ = true;
    ok = PublishLocked();
    // v2: натив (quality) или переиспользование готового 720p-кэша.
    if (v2open_ && cacheV2_.size() >= (size_t)vcam::VCamV2MaxFrameSize) {
        uint32_t tw = 0, th = 0;
        if (ResolveV2Size(w, h, quality_, tw, th)) {
            if (tw == vcam::VCamWidth && th == vcam::VCamHeight) {
                // зеркало 720p: копия не нужна, V2SrcLocked берёт cache_
                v2Stride_ = vcam::VCamStride;
            } else if (tw == w && th == h && stride == (int)(w * 4)) {
                memcpy(cacheV2_.data(), bgrx, (size_t)h * w * 4);
                v2Stride_ = w * 4;
            } else {
                v2Stride_ = tw * 4;
                vcam::LetterboxBilinearEx(bgrx, w, h, (LONG)stride, cacheV2_.data(),
                                          tw, th, (LONG)v2Stride_);
            }
            v2W_ = tw;
            v2H_ = th;
            hasV2Frame_ = true;
            PublishLockedV2();
        }
    }
    }

    if (!ok) {
        LogWriter(L"WriteFrameNative: publish failed");
        return false;
    }
    Pace();
    return true;
}

bool FrameWriter::FlushLast()
{
    if (!open_) {
        LogWriter(L"FlushLast: writer is not open");
        return false;
    }

    bool ok;
    {
        ATL::CComCritSecLock<ATL::CComAutoCriticalSection> guard(cs_);
        ok = hasFrame_ && PublishLocked();
        if (v2open_ && hasV2Frame_) PublishLockedV2();
    }

    if (!ok) {
        LogWriter(L"FlushLast: no cached frame");
        return false;
    }
    Pace();
    return true;
}
