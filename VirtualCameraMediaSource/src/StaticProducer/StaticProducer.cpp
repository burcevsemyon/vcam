#include <windows.h>
#include <wincodec.h>
#include <sddl.h>
#include <cstdio>
#include <cwchar>
#include "../Common/SharedMemoryContract.h"

#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "advapi32.lib")

bool LoadAndScaleImage(const wchar_t* filePath, BYTE* pTargetBuffer, UINT targetWidth, UINT targetHeight, UINT targetStride)
{
    CoInitialize(nullptr);

    IWICImagingFactory* pFactory = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pFactory));
    if (FAILED(hr)) return false;

    IWICBitmapDecoder* pDecoder = nullptr;
    hr = pFactory->CreateDecoderFromFilename(filePath, nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &pDecoder);
    if (FAILED(hr)) { pFactory->Release(); CoUninitialize(); return false; }

    IWICBitmapFrameDecode* pFrame = nullptr;
    hr = pDecoder->GetFrame(0, &pFrame);
    if (FAILED(hr)) { pDecoder->Release(); pFactory->Release(); CoUninitialize(); return false; }

    IWICFormatConverter* pConverter = nullptr;
    hr = pFactory->CreateFormatConverter(&pConverter);
    if (SUCCEEDED(hr)) {
        hr = pConverter->Initialize(pFrame, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom);
    }

    IWICBitmapScaler* pScaler = nullptr;
    if (SUCCEEDED(hr)) {
        hr = pFactory->CreateBitmapScaler(&pScaler);
        if (SUCCEEDED(hr)) {
            hr = pScaler->Initialize(pConverter, targetWidth, targetHeight, WICBitmapInterpolationModeHighQualityCubic);
        }
    }

    if (SUCCEEDED(hr)) {
        hr = pScaler->CopyPixels(nullptr, targetStride, targetWidth * targetHeight * 4, pTargetBuffer);
    }

    if (pScaler) pScaler->Release();
    if (pConverter) pConverter->Release();
    if (pFrame) pFrame->Release();
    if (pDecoder) pDecoder->Release();
    pFactory->Release();
    CoUninitialize();

    return SUCCEEDED(hr);
}

int wmain(int argc, wchar_t* argv[])
{
    if (argc < 2) {
        wprintf(L"Usage: StaticProducer.exe <path_to_image>\n");
        return 1;
    }

    const wchar_t* imagePath = argv[1];
    wprintf(L"Loading static image: %s\n", imagePath);

    BYTE* pStaticFrame = new BYTE[vcam::VCamFrameSize];
    if (!LoadAndScaleImage(imagePath, pStaticFrame, vcam::VCamWidth, vcam::VCamHeight, vcam::VCamStride)) {
        wprintf(L"Failed to load or scale image!\n");
        delete[] pStaticFrame;
        return 1;
    }

    PSECURITY_DESCRIPTOR pSecDesc = nullptr;
    ConvertStringSecurityDescriptorToSecurityDescriptorW(vcam::VCamDacSddl, SDDL_REVISION_1, &pSecDesc, nullptr);
    SECURITY_ATTRIBUTES sa = { sizeof(sa), pSecDesc, FALSE };

    SIZE_T totalSize = sizeof(vcam::VCamSectionHeader) + (SIZE_T)vcam::VCamSlotCount * vcam::VCamFrameSize;
    HANDLE hSection = CreateFileMappingW(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE,
        (DWORD)(totalSize >> 32), (DWORD)(totalSize & 0xFFFFFFFF), vcam::VCamSectionName);

    if (!hSection) {
        wprintf(L"CreateFileMappingW failed: %lu\n", GetLastError());
        LocalFree(pSecDesc);
        delete[] pStaticFrame;
        return 1;
    }

    HANDLE hReadyEvent = CreateEventW(&sa, FALSE, FALSE, vcam::VCamReadyEventName);
    BYTE* pBase = (BYTE*)MapViewOfFileEx(hSection, FILE_MAP_ALL_ACCESS, 0, 0, totalSize, nullptr);

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

    wprintf(L"Static Producer running @ 30 FPS. Press Ctrl+C or Esc to stop.\n");

    LARGE_INTEGER freq, counter, startTime;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&startTime);

    UINT32 frameIndex = 0;
    while (true) {
        if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) break;

        UINT32 slot = (pHeader->frameWriteIndex + 1) % vcam::VCamSlotCount;
        BYTE* pFrameSlot = pBase + sizeof(vcam::VCamSectionHeader) + (SIZE_T)slot * vcam::VCamFrameSize;

        _InterlockedExchangeAdd64((volatile LONGLONG*)&pHeader->seq, 1);
        memcpy(pFrameSlot, pStaticFrame, vcam::VCamFrameSize);
        pHeader->frameWriteIndex = slot;
        QueryPerformanceCounter(&counter);
        pHeader->lastFrameTime100ns = (UINT64)((counter.QuadPart - startTime.QuadPart) * 10000000 / freq.QuadPart);
        _InterlockedExchangeAdd64((volatile LONGLONG*)&pHeader->seq, 1);

        SetEvent(hReadyEvent);
        frameIndex++;
        Sleep(33);
    }

    UnmapViewOfFile(pBase);
    CloseHandle(hReadyEvent);
    CloseHandle(hSection);
    LocalFree(pSecDesc);
    delete[] pStaticFrame;
    return 0;
}
