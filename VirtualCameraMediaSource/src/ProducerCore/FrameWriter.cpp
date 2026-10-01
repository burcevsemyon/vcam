#include "FrameWriter.h"

#include <sddl.h>

#pragma comment(lib, "advapi32.lib")

namespace {

void LogWriter(const std::wstring& msg)
{
    OutputDebugStringW((L"[ProducerCore] " + msg + L"\n").c_str());
}

constexpr LONGLONG kFrameIntervalMs = 33; // 30 FPS

} // namespace

FrameWriter::FrameWriter()
{
    InitializeCriticalSection(&cs_);
    csInit_ = true;
    QueryPerformanceFrequency(&freq_);
    cache_.resize(vcam::VCamFrameSize);
}

FrameWriter::~FrameWriter()
{
    Close();
    if (csInit_) { DeleteCriticalSection(&cs_); csInit_ = false; }
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
    hSection_ = CreateFileMappingW(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE,
        (DWORD)(totalSize >> 32), (DWORD)(totalSize & 0xFFFFFFFF), vcam::VCamSectionName);
    if (hSection_) {
        openedSection_ = vcam::VCamSectionName;
    } else {
        DWORD createErr = GetLastError();
        // Limited-токен (автозапуск из HKCU Run) без SeCreateGlobalPrivilege не может
        // СОЗДАВАТЬ Global\-объекты (ERROR_ACCESS_DENIED). Открыть уже существующую
        // секцию и создать сеансовую Local\-копию при этом можно — читатели
        // (SharedMemoryFrameSource) перебирают Global\ -> Local\ сами.
        hSection_ = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, vcam::VCamSectionName);
        if (hSection_) {
            openedSection_ = vcam::VCamSectionName;
            LogWriter(L"section: opened existing Global after create failed: " +
                      std::to_wstring(createErr));
        } else {
            const std::wstring localName = L"Local\\" + baseName;
            hSection_ = CreateFileMappingW(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE,
                (DWORD)(totalSize >> 32), (DWORD)(totalSize & 0xFFFFFFFF), localName.c_str());
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

    hReady_ = CreateEventW(&sa, FALSE, FALSE, vcam::VCamReadyEventName);
    if (hReady_ == nullptr) {
        // Нет SeCreateGlobalPrivilege — пробуем открыть уже существующее событие
        // (созданное держателем/читателем); без него читатель переходит в poll-режим.
        DWORD createErr = GetLastError();
        hReady_ = OpenEventW(EVENT_MODIFY_STATE | SYNCHRONIZE, FALSE, vcam::VCamReadyEventName);
        if (hReady_ == nullptr && openedSection_ != vcam::VCamSectionName) {
            // Секция в Local\ — событие тоже создаём в Local\ (читатели перебирают префиксы).
            const wchar_t* pSepE = wcschr(vcam::VCamReadyEventName, L'\\');
            const std::wstring baseEvent = (pSepE != nullptr) ? pSepE + 1 : vcam::VCamReadyEventName;
            const std::wstring localEvent = L"Local\\" + baseEvent;
            hReady_ = CreateEventW(&sa, FALSE, FALSE, localEvent.c_str());
        }
        if (hReady_ == nullptr) {
            LogWriter(L"ready event unavailable: create=" + std::to_wstring(createErr) +
                      L" open=" + std::to_wstring(GetLastError()));
        }
    }
    pBase_ = (uint8_t*)MapViewOfFileEx(hSection_, FILE_MAP_ALL_ACCESS, 0, 0, totalSize, nullptr);
    if (!pBase_) {
        err = L"MapViewOfFileEx failed: " + std::to_wstring(GetLastError());
        CloseHandle(hReady_); hReady_ = nullptr;
        CloseHandle(hSection_); hSection_ = nullptr;
        LocalFree(pSecDesc_); pSecDesc_ = nullptr;
        return false;
    }

    pHeader_ = (vcam::VCamSectionHeader*)pBase_;
    pHeader_->magic = vcam::VCamMagic;
    pHeader_->version = vcam::VCamVersion;
    pHeader_->width = vcam::VCamWidth;
    pHeader_->height = vcam::VCamHeight;
    pHeader_->stride = vcam::VCamStride;
    pHeader_->pixelFormat = (UINT32)vcam::VCamPixelFormat::RGB32;
    pHeader_->frameSize = vcam::VCamFrameSize;
    pHeader_->slotCount = vcam::VCamSlotCount;
    pHeader_->frameWriteIndex = 0;
    pHeader_->seq = 0;
    pHeader_->lastFrameTime100ns = 0;
    pHeader_->readerLastActiveTick = 0;

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
    if (pBase_) { UnmapViewOfFile(pBase_); pBase_ = nullptr; }
    pHeader_ = nullptr;
    if (hReady_) { CloseHandle(hReady_); hReady_ = nullptr; }
    if (hSection_) { CloseHandle(hSection_); hSection_ = nullptr; }
    if (pSecDesc_) { LocalFree(pSecDesc_); pSecDesc_ = nullptr; }
    open_ = false;
    hasFrame_ = false;
}

bool FrameWriter::PublishLocked()
{
    if (!open_ || !pBase_ || !pHeader_) return false;

    UINT32 slot = (pHeader_->frameWriteIndex + 1) % vcam::VCamSlotCount;
    uint8_t* dst = pBase_ + sizeof(vcam::VCamSectionHeader) + (SIZE_T)slot * vcam::VCamFrameSize;

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

void FrameWriter::Pace()
{
    if (freq_.QuadPart == 0) return;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    LONGLONG interval = freq_.QuadPart * kFrameIntervalMs / 1000;
    LONGLONG target = lastEmit_ + interval;
    if (target > now.QuadPart) {
        DWORD ms = (DWORD)((target - now.QuadPart) * 1000 / freq_.QuadPart);
        if (ms > 0) Sleep(ms);
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
    EnterCriticalSection(&cs_);
    if (stride == (int)vcam::VCamStride) {
        memcpy(cache_.data(), bgrx, vcam::VCamFrameSize);
    } else {
        for (UINT32 y = 0; y < vcam::VCamHeight; y++) {
            memcpy(cache_.data() + (SIZE_T)y * vcam::VCamStride,
                   bgrx + (SIZE_T)y * (size_t)stride, vcam::VCamStride);
        }
    }
    hasFrame_ = true;
    ok = PublishLocked();
    LeaveCriticalSection(&cs_);

    if (!ok) {
        LogWriter(L"WriteFrame: publish failed");
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
    EnterCriticalSection(&cs_);
    ok = hasFrame_ && PublishLocked();
    LeaveCriticalSection(&cs_);

    if (!ok) {
        LogWriter(L"FlushLast: no cached frame");
        return false;
    }
    Pace();
    return true;
}
