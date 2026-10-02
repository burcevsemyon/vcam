#include <windows.h>
#include <sddl.h>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include "../Common/SharedMemoryContract.h"
#include "../Common/MappedViewOfFilePtr.h"

#pragma comment(lib, "advapi32.lib")

static constexpr const wchar_t* SectionName = vcam::VCamSectionName;
static constexpr const wchar_t* ReadyEventName = vcam::VCamReadyEventName;
static constexpr const wchar_t* Sddl = vcam::VCamDacSddl;

int wmain(int argc, wchar_t* argv[])
{
    wprintf(L"VCam Producer Test - writes test pattern to shared memory\n");
    wprintf(L"Press Ctrl+C to stop.\n\n");

    // Setup security descriptor
    PSECURITY_DESCRIPTOR pSecDesc = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(Sddl, SDDL_REVISION_1, &pSecDesc, nullptr)) {
        wprintf(L"ConvertStringSecurityDescriptorToSecurityDescriptorW failed: %lu\n", GetLastError());
        return 1;
    }
    SECURITY_ATTRIBUTES sa = {};
    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = pSecDesc;
    sa.bInheritHandle = FALSE;

    SIZE_T totalSize = sizeof(vcam::VCamSectionHeader) + (SIZE_T)vcam::VCamSlotCount * vcam::VCamFrameSize;
    HANDLE hSection = CreateFileMappingW(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE,
        (DWORD)(totalSize >> 32), (DWORD)(totalSize & 0xFFFFFFFF), SectionName);
    if (hSection == nullptr) {
        wprintf(L"CreateFileMappingW failed: %lu\n", GetLastError());
        LocalFree(pSecDesc);
        return 1;
    }

    HANDLE hReadyEvent = CreateEventW(&sa, FALSE, FALSE, ReadyEventName);
    if (hReadyEvent == nullptr) {
        wprintf(L"CreateEventW failed: %lu\n", GetLastError());
        CloseHandle(hSection);
        LocalFree(pSecDesc);
        return 1;
    }

    vcam::MappedViewOfFilePtr view(MapViewOfFileEx(hSection, FILE_MAP_ALL_ACCESS, 0, 0, totalSize, nullptr));
    BYTE* pBase = (BYTE*)view.Get();
    if (pBase == nullptr) {
        wprintf(L"MapViewOfFileEx failed: %lu\n", GetLastError());
        CloseHandle(hReadyEvent);
        CloseHandle(hSection);
        LocalFree(pSecDesc);
        return 1;
    }

    vcam::VCamSectionHeader* pHeader = (vcam::VCamSectionHeader*)pBase;
    pHeader->magic = vcam::VCamMagic;
    pHeader->version = vcam::VCamVersion;
    pHeader->width = vcam::VCamWidth;
    pHeader->height = vcam::VCamHeight;
    pHeader->stride = vcam::VCamStride;
    pHeader->pixelFormat = (UINT32)vcam::VCamPixelFormat::RGB32;
    pHeader->frameSize = vcam::VCamFrameSize;
    pHeader->slotCount = vcam::VCamSlotCount;
    pHeader->frameWriteIndex = 0;
    pHeader->seq = 0;
    pHeader->lastFrameTime100ns = 0;

    // Draw initial frame
    for (UINT32 y = 0; y < vcam::VCamHeight; ++y) {
        for (UINT32 x = 0; x < vcam::VCamWidth; ++x) {
            BYTE* pPixel = pBase + sizeof(vcam::VCamSectionHeader) + ((SIZE_T)y * vcam::VCamStride + (SIZE_T)x * 4);
            pPixel[0] = (BYTE)(x & 0xFF);       // R
            pPixel[1] = (BYTE)(y & 0xFF);       // G
            pPixel[2] = (BYTE)((x + y) & 0xFF); // B
            pPixel[3] = 0xFF;                   // A
        }
    }

    wprintf(L"Ready. Frame size: %lu x %lu, stride: %lu, frame bytes: %lu\n",
        vcam::VCamWidth, vcam::VCamHeight, vcam::VCamStride, vcam::VCamFrameSize);
    wprintf(L"Section: %s\n", SectionName);
    wprintf(L"Event: %s\n\n", ReadyEventName);

    UINT32 frameIndex = 0;
    LARGE_INTEGER freq, counter;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&counter);
    LARGE_INTEGER startTime = counter;

    while (true) {
        if (GetAsyncKeyState(0x03) & 0x8000) {
            wprintf(L"\nCtrl+C detected. Stopping.\n");
            break;
        }

        // Draw frame
        UINT32 slot = (pHeader->frameWriteIndex + 1) % vcam::VCamSlotCount;
        BYTE* pFrame = pBase + sizeof(vcam::VCamSectionHeader) + (SIZE_T)slot * vcam::VCamFrameSize;

        // Seqlock: odd = writing
        _InterlockedExchangeAdd64((volatile LONGLONG*)&pHeader->seq, 1);

        // Draw test pattern: moving color block
        for (UINT32 y = 0; y < vcam::VCamHeight; ++y) {
            for (UINT32 x = 0; x < vcam::VCamWidth; ++x) {
                BYTE* pPixel = pFrame + ((SIZE_T)y * vcam::VCamStride + (SIZE_T)x * 4);
                // Moving gradient
                int bx = (int)((x + frameIndex * 10) % 256);
                int by = (int)((y + frameIndex * 5) % 256);
                pPixel[0] = (BYTE)bx;       // R
                pPixel[1] = (BYTE)by;       // G
                pPixel[2] = (BYTE)((bx + by) / 2); // B
                pPixel[3] = 0xFF;           // A
            }
        }

        // Update frame index and timestamp
        pHeader->frameWriteIndex = slot;
        QueryPerformanceCounter(&counter);
        pHeader->lastFrameTime100ns = (UINT64)((counter.QuadPart - startTime.QuadPart) * 10000000 / freq.QuadPart);

        // Seqlock: even = stable
        _InterlockedExchangeAdd64((volatile LONGLONG*)&pHeader->seq, 1);

        // Signal ready
        SetEvent(hReadyEvent);

        frameIndex++;

        // Sleep for ~33ms (30 fps)
        Sleep(33);
    }

    wprintf(L"Total frames written: %lu\n", frameIndex);

    // Cleanup
    view.Close();
    CloseHandle(hReadyEvent);
    CloseHandle(hSection);
    LocalFree(pSecDesc);
    return 0;
}
