#pragma once
// The local SDK declares IMFSampleAllocatorControl only inside the
// WINAPI_PARTITION_APP (WinRT) section of mfidl.h, which a desktop build
// cannot see. This mirrors that SDK declaration exactly:
//   GUID DA62B958-3A38-4A97-BD27-149C640C0771
//   MFSampleAllocatorUsage { UsesProvidedAllocator=0, UsesCustomAllocator=1, DoesNotAllocate=2 }
#include <unknwn.h>

#ifndef __IMFSampleAllocatorControl_INTERFACE_DEFINED__
#define __IMFSampleAllocatorControl_INTERFACE_DEFINED__

typedef
enum MFSampleAllocatorUsage
{
    MFSampleAllocatorUsage_UsesProvidedAllocator = 0,
    MFSampleAllocatorUsage_UsesCustomAllocator = (MFSampleAllocatorUsage_UsesProvidedAllocator + 1),
    MFSampleAllocatorUsage_DoesNotAllocate = (MFSampleAllocatorUsage_UsesCustomAllocator + 1)
} MFSampleAllocatorUsage;

MIDL_INTERFACE("DA62B958-3A38-4A97-BD27-149C640C0771")
IMFSampleAllocatorControl : public IUnknown
{
public:
    virtual HRESULT STDMETHODCALLTYPE SetDefaultAllocator(
        DWORD dwOutputStreamID,
        IUnknown* pAllocator) = 0;

    virtual HRESULT STDMETHODCALLTYPE GetAllocatorUsage(
        DWORD dwOutputStreamID,
        DWORD* pdwInputStreamID,
        MFSampleAllocatorUsage* peUsage) = 0;
};

#endif // __IMFSampleAllocatorControl_INTERFACE_DEFINED__
