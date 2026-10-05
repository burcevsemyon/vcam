#include <windows.h>
#include <wincodec.h>
#include <atlbase.h>
#include <sddl.h>
#include <cstdio>
#include <cwchar>
#include <cmath>
#include <string>
#include <vector>
#include <memory>
#include <new>
#include "../Common/SharedMemoryContract.h"
#include "../Common/MappedViewOfFilePtr.h"
#include "../Common/CriticalSectionGuard.h"

#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "advapi32.lib")

// Scale modes must stay in sync with VCamSettingsUi (C#) preview rendering.
enum class ScaleMode { Fit, Cover, Crop };

struct CropRect {
    int x = 0, y = 0, w = 0, h = 0;
    bool operator==(const CropRect& o) const { return x == o.x && y == o.y && w == o.w && h == o.h; }
    bool IsZero() const { return w <= 0 || h <= 0; }
};

struct Settings {
    std::wstring imagePath;
    ScaleMode mode = ScaleMode::Fit;
    CropRect crop;
    bool cropKeepAspect = false; // crop mode: letterbox instead of stretch
    std::wstring mediaPath;      // new (written by VCamSettingsUi)
    std::wstring mediaMode;      // "static" | "video"
};

static std::unique_ptr<BYTE[]> g_pFrame;
static CRITICAL_SECTION g_frameCs;
static HANDLE g_hStopEvent = nullptr;
static std::wstring g_currentImagePath;
static ScaleMode g_currentMode = ScaleMode::Fit;
static CropRect g_currentCrop;
static bool g_currentCropKeepAspect = false;
static std::wstring g_currentMediaMode;

static std::wstring SettingsDirPath()
{
    wchar_t appdata[MAX_PATH] = {};
    DWORD n = GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return std::wstring();
    return std::wstring(appdata) + L"\\VCam";
}

static std::wstring SettingsFilePath()
{
    std::wstring dir = SettingsDirPath();
    if (dir.empty()) return std::wstring();
    return dir + L"\\settings.json";
}

static bool ReadUtf8File(const std::wstring& path, std::string& out)
{
    HANDLE raw = CreateFileW(path.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (raw == INVALID_HANDLE_VALUE) return false;
    ATL::CHandle h(raw);
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart <= 0 || sz.QuadPart > 1000000) return false;
    out.resize((size_t)sz.QuadPart);
    DWORD read = 0;
    BOOL ok = ReadFile(h, &out[0], (DWORD)out.size(), &read, nullptr);
    return ok && read == out.size();
}

static std::wstring Utf8ToWide(const std::string& s)
{
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

// Minimal flat-JSON string extractor: finds "key" then the quoted value after ':'.
// Handles standard escapes (\\ \" \/ \n \r \t \b \f). Sufficient for our 2-key settings file.
static bool JsonGetString(const std::string& json, const char* key, std::wstring& value)
{
    std::string k = std::string("\"") + key + "\"";
    size_t p = json.find(k);
    if (p == std::string::npos) return false;
    p = json.find(':', p + k.size());
    if (p == std::string::npos) return false;
    p++;
    while (p < json.size() && (json[p] == ' ' || json[p] == '\t' || json[p] == '\r' || json[p] == '\n')) p++;
    if (p >= json.size() || json[p] != '"') return false;
    std::string val;
    for (p++; p < json.size() && json[p] != '"'; p++) {
        if (json[p] == '\\' && p + 1 < json.size()) {
            char c = json[++p];
            switch (c) {
                case 'n': val += '\n'; break;
                case 'r': val += '\r'; break;
                case 't': val += '\t'; break;
                case 'b': val += '\b'; break;
                case 'f': val += '\f'; break;
                case 'u': // skip 4 hex digits (we do not expect non-ASCII in keys/paths we write)
                    if (p + 4 < json.size()) p += 4;
                    val += '?';
                    break;
                default: val += c; break; // covers \\ \" \/
            }
        } else {
            val += json[p];
        }
    }
    if (p >= json.size()) return false;
    value = Utf8ToWide(val);
    return true;
}

// Extracts an integer value for "key": <number>. Returns defaultVal when absent.
static int JsonGetInt(const std::string& json, const char* key, int defaultVal)
{
    std::string k = std::string("\"") + key + "\"";
    size_t p = json.find(k);
    if (p == std::string::npos) return defaultVal;
    p = json.find(':', p + k.size());
    if (p == std::string::npos) return defaultVal;
    p++;
    while (p < json.size() && (json[p] == ' ' || json[p] == '\t' || json[p] == '\r' || json[p] == '\n')) p++;
    bool neg = false;
    if (p < json.size() && json[p] == '-') { neg = true; p++; }
    if (p >= json.size() || json[p] < '0' || json[p] > '9') return defaultVal;
    long v = 0;
    for (; p < json.size() && json[p] >= '0' && json[p] <= '9'; p++) {
        v = v * 10 + (json[p] - '0');
        if (v > 1000000) break;
    }
    return neg ? (int)-v : (int)v;
}

// Extracts a bool value for "key": true|false. Returns defaultVal when absent.
static bool JsonGetBool(const std::string& json, const char* key, bool defaultVal)
{
    std::string k = std::string("\"") + key + "\"";
    size_t p = json.find(k);
    if (p == std::string::npos) return defaultVal;
    p = json.find(':', p + k.size());
    if (p == std::string::npos) return defaultVal;
    p++;
    while (p < json.size() && (json[p] == ' ' || json[p] == '\t' || json[p] == '\r' || json[p] == '\n')) p++;
    if (p + 4 <= json.size() && json.compare(p, 4, "true") == 0) return true;
    if (p + 5 <= json.size() && json.compare(p, 5, "false") == 0) return false;
    return defaultVal;
}

// mediaPath is preferred when present, unless mediaMode says the file is a
// video ("video" belongs to VideoProducer - warn and fall back to imagePath).
static std::wstring EffectiveImagePath(const Settings& s)
{
    if (!s.mediaPath.empty() && s.mediaMode != L"video") return s.mediaPath;
    if (!s.imagePath.empty()) return s.imagePath;
    return s.mediaPath;
}

static bool LoadSettings(Settings& s)
{
    std::wstring path = SettingsFilePath();
    if (path.empty()) return false;
    std::string json;
    if (!ReadUtf8File(path, json)) return false;
    JsonGetString(json, "imagePath", s.imagePath);
    JsonGetString(json, "mediaPath", s.mediaPath);
    JsonGetString(json, "mediaMode", s.mediaMode);
    std::wstring mode;
    if (JsonGetString(json, "scaleMode", mode)) {
        if (mode == L"cover") s.mode = ScaleMode::Cover;
        else if (mode == L"crop") s.mode = ScaleMode::Crop;
        else s.mode = ScaleMode::Fit;
    }
    if (s.mode == ScaleMode::Crop) {
        s.crop.x = JsonGetInt(json, "cropX", 0);
        s.crop.y = JsonGetInt(json, "cropY", 0);
        s.crop.w = JsonGetInt(json, "cropW", 0);
        s.crop.h = JsonGetInt(json, "cropH", 0);
        s.cropKeepAspect = JsonGetBool(json, "cropKeepAspect", false);
    }
    // Normalize so all downstream code (startup, watcher, hot-reload) works
    // with a single effective source path, exactly as before these fields existed.
    s.imagePath = EffectiveImagePath(s);
    return !s.imagePath.empty();
}

static void WarnIfVideoMode(const Settings& s)
{
    if (s.mediaMode == L"video") {
        wprintf(L"[settings] WARNING: mediaMode=video but StaticProducer is running - "
                L"continuing with the static image: %s\n", s.imagePath.c_str());
        fflush(stdout);
    }
}

static const wchar_t* ModeName(ScaleMode m)
{
    switch (m) {
        case ScaleMode::Cover: return L"cover";
        case ScaleMode::Crop:  return L"crop";
        default:               return L"fit";
    }
}

// Loads an image, converts to 32bppBGRA and renders it into a 1280x720 RGB32 frame:
//   Fit   - scale by min(w/tw, h/th), letterbox on black background
//   Cover - scale by max(w/tw, h/th), center-crop to target
//   Crop  - clip settings crop rect (source pixels); stretch to target,
//           or letterbox it when keepAspect is set
static bool LoadAndScaleImage(const wchar_t* filePath, ScaleMode mode, CropRect crop, bool keepAspect,
                              BYTE* pTargetBuffer, UINT tw, UINT th, UINT tstride)
{
    CoInitialize(nullptr);

    ATL::CComPtr<IWICImagingFactory> pFactory;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pFactory));
    if (FAILED(hr)) { pFactory = nullptr; CoUninitialize(); return false; }

    ATL::CComPtr<IWICBitmapDecoder> pDecoder;
    hr = pFactory->CreateDecoderFromFilename(filePath, nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &pDecoder);
    if (FAILED(hr)) { pDecoder = nullptr; pFactory = nullptr; CoUninitialize(); return false; }

    ATL::CComPtr<IWICBitmapFrameDecode> pFrame;
    hr = pDecoder->GetFrame(0, &pFrame);
    if (FAILED(hr)) { pFrame = nullptr; pDecoder = nullptr; pFactory = nullptr; CoUninitialize(); return false; }

    UINT sw = 0, sh = 0;
    pFrame->GetSize(&sw, &sh);
    if (sw == 0 || sh == 0) { pFrame = nullptr; pDecoder = nullptr; pFactory = nullptr; CoUninitialize(); return false; }

    ATL::CComPtr<IWICFormatConverter> pConverter;
    hr = pFactory->CreateFormatConverter(&pConverter);
    if (SUCCEEDED(hr)) {
        hr = pConverter->Initialize(pFrame, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom);
    }

    IWICBitmapSource* pSource = pConverter;
    ATL::CComPtr<IWICBitmapClipper> pCropClipper;
    UINT srcW = sw, srcH = sh;

    if (SUCCEEDED(hr) && mode == ScaleMode::Crop) {
        // Clamp crop rect to source bounds; fall back to full image if invalid.
        WICRect rc = { 0, 0, (INT)sw, (INT)sh };
        if (!crop.IsZero()) {
            int x = crop.x < 0 ? 0 : crop.x;
            int y = crop.y < 0 ? 0 : crop.y;
            if (x >= (INT)sw) x = 0;
            if (y >= (INT)sh) y = 0;
            int w = crop.w, h = crop.h;
            if (w > (INT)sw - x) w = (INT)sw - x;
            if (h > (INT)sh - y) h = (INT)sh - y;
            if (w > 0 && h > 0) { rc = { x, y, w, h }; }
        }
        hr = pFactory->CreateBitmapClipper(&pCropClipper);
        if (SUCCEEDED(hr)) hr = pCropClipper->Initialize(pConverter, &rc);
        if (SUCCEEDED(hr)) { pSource = pCropClipper; srcW = (UINT)rc.Width; srcH = (UINT)rc.Height; }
        else pSource = nullptr;
    }

    UINT nw = sw, nh = sh;
    if (SUCCEEDED(hr) && pSource && mode != ScaleMode::Crop) {
        double scale = (mode == ScaleMode::Fit)
            ? ((double)tw < (double)th * sw / sh ? (double)tw / sw : (double)th / sh)
            : ((double)tw > (double)th * sw / sh ? (double)tw / sw : (double)th / sh);
        // Fit: never exceed target (fp rounding guard); Cover: never go below target.
        nw = (UINT)llround(sw * scale);
        nh = (UINT)llround(sh * scale);
        if (mode == ScaleMode::Fit) { if (nw > tw) nw = tw; if (nh > th) nh = th; }
        else { if (nw < tw) nw = tw; if (nh < th) nh = th; }
        if (nw == 0) nw = 1;
        if (nh == 0) nh = 1;
    }

    // Crop: stretch clipped region to full target (aspect NOT preserved),
    // or scale it to fit inside the target (letterbox) when keepAspect is set.
    if (SUCCEEDED(hr) && mode == ScaleMode::Crop) {
        if (keepAspect) {
            double scale = ((double)tw / srcW < (double)th / srcH) ? (double)tw / srcW : (double)th / srcH;
            nw = (UINT)llround(srcW * scale);
            nh = (UINT)llround(srcH * scale);
            if (nw > tw) nw = tw;
            if (nh > th) nh = th;
            if (nw == 0) nw = 1;
            if (nh == 0) nh = 1;
        } else {
            nw = tw; nh = th;
        }
    }

    ATL::CComPtr<IWICBitmapScaler> pScaler;
    if (SUCCEEDED(hr) && pSource && (nw != srcW || nh != srcH)) {
        hr = pFactory->CreateBitmapScaler(&pScaler);
        if (SUCCEEDED(hr)) {
            hr = pScaler->Initialize(pSource, nw, nh, WICBitmapInterpolationModeHighQualityCubic);
            if (SUCCEEDED(hr)) pSource = pScaler;
        }
    }

    bool ok = false;
    if (SUCCEEDED(hr) && pSource) {
        if (mode == ScaleMode::Fit || (mode == ScaleMode::Crop && keepAspect)) {
            std::vector<BYTE> tmp((size_t)nw * nh * 4);
            if (SUCCEEDED(pSource->CopyPixels(nullptr, nw * 4, (UINT)tmp.size(), tmp.data()))) {
                memset(pTargetBuffer, 0, (size_t)tstride * th);
                UINT xoff = (tw - nw) / 2;
                UINT yoff = (th - nh) / 2;
                for (UINT y = 0; y < nh; y++) {
                    memcpy(pTargetBuffer + (size_t)(yoff + y) * tstride + (size_t)xoff * 4,
                           &tmp[(size_t)y * nw * 4], (size_t)nw * 4);
                }
                ok = true;
            }
        } else {
            ATL::CComPtr<IWICBitmapClipper> pClipper;
            if (SUCCEEDED(pFactory->CreateBitmapClipper(&pClipper))) {
                WICRect rc = { (INT)((nw - tw) / 2), (INT)((nh - th) / 2), (INT)tw, (INT)th };
                if (SUCCEEDED(pClipper->Initialize(pSource, &rc))) {
                    ok = SUCCEEDED(pClipper->CopyPixels(nullptr, tstride, tstride * th, pTargetBuffer));
                }
            }
        }
    }

    pScaler = nullptr;
    pCropClipper = nullptr;
    pConverter = nullptr;
    pFrame = nullptr;
    pDecoder = nullptr;
    pFactory = nullptr;
    CoUninitialize();

    return ok;
}

static bool TryLoadIntoBuffer(const Settings& s, BYTE* pBuffer)
{
    if (s.imagePath.empty()) return false;
    if (!LoadAndScaleImage(s.imagePath.c_str(), s.mode, s.crop, s.cropKeepAspect, pBuffer, vcam::VCamWidth, vcam::VCamHeight, vcam::VCamStride)) {
        wprintf(L"[settings] failed to load image: %s\n", s.imagePath.c_str());
        fflush(stdout);
        return false;
    }
    return true;
}

// Polls settings.json every 500ms; on change reloads image+mode without restarting.
static DWORD WINAPI SettingsWatcherThread(LPVOID)
{
    std::wstring path = SettingsFilePath();
    ULARGE_INTEGER lastWrite = {};
    for (;;) {
        if (WaitForSingleObject(g_hStopEvent, 500) != WAIT_TIMEOUT) break;

        WIN32_FILE_ATTRIBUTE_DATA fad;
        if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad)) {
            lastWrite.QuadPart = 0;
            continue;
        }
        ULARGE_INTEGER ft;
        ft.LowPart = fad.ftLastWriteTime.dwLowDateTime;
        ft.HighPart = fad.ftLastWriteTime.dwHighDateTime;
        if (ft.QuadPart == lastWrite.QuadPart) continue;

        Sleep(200); // debounce: let the writer finish
        lastWrite = ft;

        Settings s;
        if (!LoadSettings(s) || s.imagePath.empty()) continue;
        bool modeChanged = (s.mediaMode != g_currentMediaMode);
        if (s.imagePath == g_currentImagePath && s.mode == g_currentMode && s.crop == g_currentCrop
            && s.cropKeepAspect == g_currentCropKeepAspect) {
            if (modeChanged) {
                g_currentMediaMode = s.mediaMode;
                WarnIfVideoMode(s);
            }
            continue;
        }

        std::unique_ptr<BYTE[]> tmp;
        try { tmp = std::make_unique_for_overwrite<BYTE[]>(vcam::VCamFrameSize); }
        catch (const std::bad_alloc&) { tmp.reset(); }
        if (!tmp) continue;
        if (TryLoadIntoBuffer(s, tmp.get())) {
            {
                vcam::CsGuard guard(&g_frameCs);
                g_pFrame = std::move(tmp);
                g_currentImagePath = s.imagePath;
                g_currentMode = s.mode;
                g_currentCrop = s.crop;
                g_currentCropKeepAspect = s.cropKeepAspect;
                g_currentMediaMode = s.mediaMode;
            }
            wprintf(L"[settings] reloaded: %s (mode=%s)\n", g_currentImagePath.c_str(), ModeName(g_currentMode));
            fflush(stdout);
            if (modeChanged) WarnIfVideoMode(s);
        } // else: tmp освобождается автоматически
    }
    return 0;
}

int wmain(int argc, wchar_t* argv[])
{
    Settings settings;
    bool haveSettings = LoadSettings(settings);

    if (argc >= 2) {
        settings.imagePath = argv[1]; // explicit CLI path overrides settings file path
    } else if (!haveSettings || settings.imagePath.empty()) {
        wprintf(L"Usage: StaticProducer.exe [path_to_image]\n");
        wprintf(L"  Settings file: %s\n", SettingsFilePath().c_str());
        wprintf(L"  { \"imagePath\": \"...\", \"scaleMode\": \"fit\"|\"cover\"|\"crop\", \"cropX\":0, \"cropY\":0, \"cropW\":0, \"cropH\":0, \"cropKeepAspect\":false }\n");
        wprintf(L"  { \"mediaPath\": \"...\", \"mediaMode\": \"static\"|\"video\" }\n");
        return 1;
    }

    if (haveSettings) WarnIfVideoMode(settings);

    wprintf(L"Loading static image: %s (mode=%s)\n", settings.imagePath.c_str(), ModeName(settings.mode));

    std::unique_ptr<BYTE[]> pFrame;
    try { pFrame = std::make_unique_for_overwrite<BYTE[]>(vcam::VCamFrameSize); }
    catch (const std::bad_alloc&) { pFrame.reset(); }
    if (!pFrame || !TryLoadIntoBuffer(settings, pFrame.get())) {
        wprintf(L"Failed to load or scale image!\n");
        return 1;
    }

    InitializeCriticalSection(&g_frameCs);
    g_pFrame = std::move(pFrame);
    g_currentImagePath = settings.imagePath;
    g_currentMode = settings.mode;
    g_currentCrop = settings.crop;
    g_currentCropKeepAspect = settings.cropKeepAspect;
    g_currentMediaMode = settings.mediaMode;
    g_hStopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);

    PSECURITY_DESCRIPTOR pSecDesc = nullptr;
    ConvertStringSecurityDescriptorToSecurityDescriptorW(vcam::VCamDacSddl, SDDL_REVISION_1, &pSecDesc, nullptr);
    SECURITY_ATTRIBUTES sa = { sizeof(sa), pSecDesc, FALSE };

    SIZE_T totalSize = sizeof(vcam::VCamSectionHeader) + (SIZE_T)vcam::VCamSlotCount * vcam::VCamFrameSize;
    ATL::CHandle hSection(CreateFileMappingW(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE,
        (DWORD)(totalSize >> 32), (DWORD)(totalSize & 0xFFFFFFFF), vcam::VCamSectionName));

    if (!hSection) {
        wprintf(L"CreateFileMappingW failed: %lu\n", GetLastError());
        LocalFree(pSecDesc);
        g_pFrame.reset();
        CloseHandle(g_hStopEvent);
        DeleteCriticalSection(&g_frameCs);
        return 1;
    }

    ATL::CHandle hReadyEvent(CreateEventW(&sa, FALSE, FALSE, vcam::VCamReadyEventName));
    vcam::MappedViewOfFilePtr view(MapViewOfFileEx(hSection, FILE_MAP_ALL_ACCESS, 0, 0, totalSize, nullptr));
    BYTE* pBase = (BYTE*)view.Get();

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

    ATL::CHandle hWatcher(CreateThread(nullptr, 0, SettingsWatcherThread, nullptr, 0, nullptr));

    wprintf(L"Static Producer running @ 30 FPS. Press Ctrl+C or Esc to stop.\n");
    fflush(stdout);

    LARGE_INTEGER freq, counter, startTime;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&startTime);

    while (true) {
        if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) break;

        UINT32 slot = (pHeader->frameWriteIndex + 1) % vcam::VCamSlotCount;
        BYTE* pFrameSlot = pBase + sizeof(vcam::VCamSectionHeader) + (SIZE_T)slot * vcam::VCamFrameSize;

        _InterlockedExchangeAdd64((volatile LONGLONG*)&pHeader->seq, 1);
        {
            vcam::CsGuard guard(&g_frameCs);
            memcpy(pFrameSlot, g_pFrame.get(), vcam::VCamFrameSize);
        }
        pHeader->frameWriteIndex = slot;
        QueryPerformanceCounter(&counter);
        pHeader->lastFrameTime100ns = (UINT64)((counter.QuadPart - startTime.QuadPart) * 10000000 / freq.QuadPart);
        _InterlockedExchangeAdd64((volatile LONGLONG*)&pHeader->seq, 1);

        SetEvent(hReadyEvent);
        Sleep(33);
    }

    SetEvent(g_hStopEvent);
    if (hWatcher) WaitForSingleObject(hWatcher, 2000);
    hWatcher.Close();

    view.Close();
    hReadyEvent.Close();
    hSection.Close();
    LocalFree(pSecDesc);
    CloseHandle(g_hStopEvent);
    DeleteCriticalSection(&g_frameCs);
    g_pFrame.reset();
    return 0;
}
