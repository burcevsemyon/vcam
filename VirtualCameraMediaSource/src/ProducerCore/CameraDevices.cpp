#include "CameraDevices.h"

#include <windows.h>

#include <mfapi.h>
#include <mfidl.h>
#include <mfobjects.h>
#include <propidl.h>

#include <cwchar>
#include <cwctype>
#include <mutex>
#include <utility>
#include <algorithm>

#include <atlbase.h>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mf.lib")

namespace {

std::mutex g_enumMutex; // порядок пар MFStartup/MFShutdown внутри функций

using DeviceEntry = std::pair<CameraDeviceInfo, IMFActivate*>;

std::wstring PropStr(IMFActivate* dev, const GUID& key)
{
    std::wstring out;
    if (!dev) return out;
    PROPVARIANT vt;
    PropVariantInit(&vt);
    if (SUCCEEDED(dev->GetItem(key, &vt))) {
        if (vt.vt == VT_LPWSTR && vt.pwszVal) out.assign(vt.pwszVal);
        else if (vt.vt == VT_BSTR && vt.bstrVal) out.assign(vt.bstrVal);
    }
    PropVariantClear(&vt);
    return out;
}

bool ContainsNoCase(const std::wstring& hay, const std::wstring& needle)
{
    if (needle.empty() || hay.size() < needle.size()) return false;
    for (size_t i = 0; i + needle.size() <= hay.size(); i++) {
        size_t j = 0;
        for (; j < needle.size(); j++)
            if (towlower(hay[i + j]) != towlower(needle[j])) break;
        if (j == needle.size()) return true;
    }
    return false;
}

bool StartsWithNoCase(const std::wstring& s, const wchar_t* prefix)
{
    size_t n = wcslen(prefix);
    if (s.size() < n) return false;
    for (size_t i = 0; i < n; i++)
        if (towlower(s[i]) != towlower(prefix[i])) return false;
    return true;
}

// Виртуальная камера (MFCreateVirtualCamera, наш Registrar) НЕ должна быть
// источником для самой себя — это петля (камера читает то, что сама пишет).
// Определение: (1) symlink, который создаёт MF-подсистема виртуальных камер,
// всегда \\?\swd#vcamdevapi#…; (2) страховка по имени — friendly name
// «VCam (…)» (cameraName из Registrar, если формат symlink изменится между
// версиями Windows). list-devices по контракту отдаёт физические камеры.
bool IsVirtualCamera(const CameraDeviceInfo& d)
{
    static constexpr wchar_t kSymlinkPrefix[] = L"\\\\?\\swd#vcamdevapi#";
    static constexpr wchar_t kNamePrefix[] = L"VCam (";
    return StartsWithNoCase(d.id, kSymlinkPrefix) || StartsWithNoCase(d.name, kNamePrefix);
}

// MFEnumDeviceSources(VIDCAP) -> пары (инфо, activate). Владение activate'ами
// переходит вызывающему. COM/MFStartup — здесь же (парно, под мьютексом).
bool EnumerateRaw(std::vector<DeviceEntry>& out, bool& comHere)
{
    comHere = false;
    HRESULT hrCo = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    comHere = (hrCo == S_OK); // RPC_E_CHANGED_MODE — COM уже инициализирован иначе: продолжаем

    HRESULT hr = MFStartup(MF_VERSION, MFSTARTUP_FULL);
    if (FAILED(hr)) return false;

    ATL::CComPtr<IMFAttributes> attr;
    IMFActivate** devs = nullptr;
    UINT32 count = 0;
    bool ok = false;
    if (SUCCEEDED(MFCreateAttributes(&attr, 1)) && attr) {
        attr->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
                      MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
        hr = MFEnumDeviceSources(attr, &devs, &count);
        attr = nullptr;
        if (SUCCEEDED(hr)) {
            out.reserve(count);
            auto readInfo = [](IMFActivate* act) {
                CameraDeviceInfo info;
                info.name = PropStr(act, MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME);
                // id = symlink устройства. Ключ VIDCAP_GUID в SDK — это значение
                // типа источника ({8AC3587A...}), а не атрибут symlink: GetItem по
                // нему возвращает MF_E_ATTRIBUTENOTFOUND. Symlink лежит в
                // VIDCAP_SYMBOLIC_LINK; VIDCAP_GUID оставляем fallback'ом.
                info.id = PropStr(act, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK);
                if (info.id.empty())
                    info.id = PropStr(act, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
                return info;
            };
            for (UINT32 i = 0; i < count; i++)
                out.emplace_back(readInfo(devs[i]), devs[i]); // владение activate
            ok = true;
        }
    }
    if (devs && !ok) { // не передали владение — освобождаем сами
        for (UINT32 i = 0; i < count; i++)
            if (devs[i]) devs[i]->Release();
    }
    if (devs) CoTaskMemFree(devs);
    return ok; // MFShutdown делает вызывающая сторона (парно)
}

void ReleaseAll(std::vector<DeviceEntry>& v)
{
    for (auto& e : v)
        if (e.second) e.second->Release();
    v.clear();
}

int MatchIndex(const std::vector<DeviceEntry>& devs, const std::wstring& id,
               const std::wstring& name)
{
    auto indexOf = [&devs](auto pred) -> int {
        auto it = std::find_if(devs.begin(), devs.end(), pred);
        return (it == devs.end()) ? -1 : static_cast<int>(it - devs.begin());
    };
    if (!id.empty()) {
        int i = indexOf([&id](const DeviceEntry& e) {
            return _wcsicmp(e.first.id.c_str(), id.c_str()) == 0;
        });
        if (i >= 0) return i;
    }
    if (!name.empty()) {
        int i = indexOf([&name](const DeviceEntry& e) {
            return _wcsicmp(e.first.name.c_str(), name.c_str()) == 0;
        });
        if (i >= 0) return i;
        return indexOf([&name](const DeviceEntry& e) {
            return ContainsNoCase(e.first.name, name);
        });
    }
    return -1;
}

} // namespace

std::vector<CameraDeviceInfo> EnumerateCameraDevices()
{
    std::vector<CameraDeviceInfo> result;
    std::lock_guard<std::mutex> lock(g_enumMutex);

    std::vector<DeviceEntry> devs;
    bool comHere = false;
    if (EnumerateRaw(devs, comHere)) {
        result.reserve(devs.size());
        for (auto& e : devs) {
            if (IsVirtualCamera(e.first)) continue; // наша виртуальная — не источник
            result.emplace_back(std::move(e.first));
        }
    }
    ReleaseAll(devs);

    MFShutdown(); // парный MFStartup этой функции
    if (comHere) CoUninitialize();
    return result;
}

IMFActivate* OpenCameraActivate(const std::wstring& id, const std::wstring& name,
                                CameraDeviceInfo& outInfo)
{
    std::lock_guard<std::mutex> lock(g_enumMutex);

    std::vector<DeviceEntry> devs;
    bool comHere = false;
    IMFActivate* result = nullptr;
    if (EnumerateRaw(devs, comHere)) {
        int idx = MatchIndex(devs, id, name);
        if (idx >= 0) {
            outInfo = std::move(devs[(size_t)idx].first);
            result = devs[(size_t)idx].second; // владение переходит вызывающему
            devs[(size_t)idx].second = nullptr;
        }
    }
    ReleaseAll(devs);

    MFShutdown(); // парный MFStartup этой функции
    if (comHere) CoUninitialize();
    return result;
}
