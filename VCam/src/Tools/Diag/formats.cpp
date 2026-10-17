#include "CaptureTestPCH.h"
#include "CaptureTestTypes.h"
#include "CaptureTestCommon.h"

int RunFormatsMode(const ModeArgs& a)
{
    MfSession session;
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr)) { LogW(L"CoInitializeEx failed: 0x%08X", hr); return 2; }
    session.comOk = true;
    hr = MFStartup(MF_VERSION);
    if (FAILED(hr)) { LogW(L"MFStartup failed: 0x%08X", hr); return 2; }
    session.mfOk = true;

    IMFActivate** ppDevices = nullptr;
    UINT32 count = 0;
    ATL::CComPtr<IMFAttributes> pEnumAttr;
    hr = MFCreateAttributes(&pEnumAttr, 1);
    if (SUCCEEDED(hr)) {
        pEnumAttr->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
                           MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
        hr = MFEnumDeviceSources(pEnumAttr, &ppDevices, &count);
        pEnumAttr = nullptr;
    }
    if (FAILED(hr)) { LogW(L"MFEnumDeviceSources: hr=0x%08X", hr); return 2; }

    int match = FindDevice(a.nameFilter, ppDevices, count);
    if (match < 0) {
        LogW(L"formats: NO DEVICE matching '%s'", a.nameFilter);
        CoTaskMemFree(ppDevices);
        return 2;
    }

    ATL::CComPtr<IMFActivate> pAct;
    pAct.Attach(ppDevices[match]);
    pAct.p->AddRef();
    CoTaskMemFree(ppDevices);

    ATL::CComPtr<IMFMediaSource> pSource;
    hr = pAct->ActivateObject(IID_IMFMediaSource, (void**)&pSource);
    if (FAILED(hr)) { LogW(L"formats: ActivateObject hr=0x%08X", hr); return 2; }

    ATL::CComPtr<IMFPresentationDescriptor> pPD;
    hr = pSource->CreatePresentationDescriptor(&pPD);
    if (FAILED(hr)) { LogW(L"formats: CreatePD hr=0x%08X", hr); pSource->Shutdown(); return 2; }

    bool has720Rgb = false, has720Nv12 = false, has640Rgb = false;
    std::vector<std::wstring> typeList;
    std::vector<std::wstring> nativeTypes;
    DWORD nStreams = 0;
    pPD->GetStreamDescriptorCount(&nStreams);
    for (DWORD s = 0; s < nStreams; ++s) {
        BOOL bSel = FALSE;
        ATL::CComPtr<IMFStreamDescriptor> pSD;
        if (FAILED(pPD->GetStreamDescriptorByIndex(s, &bSel, &pSD)) || !pSD) continue;
        ATL::CComPtr<IMFMediaTypeHandler> pMTH;
        if (FAILED(pSD->GetMediaTypeHandler(&pMTH)) || !pMTH) continue;
        DWORD mtCount = 0;
        pMTH->GetMediaTypeCount(&mtCount);
        for (DWORD m = 0; m < mtCount; ++m) {
            ATL::CComPtr<IMFMediaType> pMT;
            if (FAILED(pMTH->GetMediaTypeByIndex(m, &pMT)) || !pMT) continue;
            GUID sub = {};
            UINT32 w = 0, h = 0;
            pMT->GetGUID(MF_MT_SUBTYPE, &sub);
            MFGetAttributeSize(pMT, MF_MT_FRAME_SIZE, &w, &h);
            wchar_t subName[16];
            if (sub == MFVideoFormat_RGB32) swprintf_s(subName, L"RGB32");
            else if (sub == MFVideoFormat_NV12) swprintf_s(subName, L"NV12");
            else swprintf_s(subName, L"{%08X-%04X-%04X}", sub.Data1, sub.Data2, sub.Data3);
            wchar_t line[128];
            swprintf_s(line, L"%s %ux%u", subName, w, h);
            typeList.push_back(line);
            if (sub == MFVideoFormat_RGB32 && w == 1280 && h == 720) has720Rgb = true;
            if (sub == MFVideoFormat_NV12 && w == 1280 && h == 720) has720Nv12 = true;
            if (sub == MFVideoFormat_RGB32 && w == 640 && h == 480) has640Rgb = true;
            if ((w > 1280 || h > 720) && !(w == 1280 && h == 720)) {
                nativeTypes.push_back(line);
            }
            LogW(L"  type[%u] %s", m, line);
        }
    }
    pSource->Shutdown();
    pSource = nullptr;
    pAct = nullptr;

    const wchar_t* verdict = (has720Rgb && has720Nv12 && has640Rgb && nativeTypes.empty())
                             ? L"OK" : L"REGRESSION";
    LogW(L"--- formats summary: device=%d types=%d rgb720=%d nv12_720=%d rgb640=%d nativeTypes=%d verdict=%s",
         match, (int)typeList.size(), (int)has720Rgb, (int)has720Nv12, (int)has640Rgb,
         (int)nativeTypes.size(), verdict);

    if (a.json) {
        wprintf(L"{\"mode\":\"formats\",\"device\":%d,\"typeCount\":%d,"
                L"\"rgb720\":%s,\"nv12_720\":%s,\"rgb640\":%s,\"nativeTypes\":[",
                match, (int)typeList.size(),
                has720Rgb ? L"true" : L"false", has720Nv12 ? L"true" : L"false",
                has640Rgb ? L"true" : L"false");
        for (size_t i = 0; i < nativeTypes.size(); ++i) {
            if (i) wprintf(L",");
            wprintf(L"\"%s\"", JsonEscape(nativeTypes[i]).c_str());
        }
        wprintf(L"],\"verdict\":\"%s\"}\n", verdict);
    }
    return verdict == L"OK" ? 0 : 1;
}