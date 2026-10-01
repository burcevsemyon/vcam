#include "StaticImageSource.h"

#include <wincodec.h>
#include <atlbase.h>

#include <cmath>
#include <cstring>

#include "SharedMemoryContract.h"

#pragma comment(lib, "windowscodecs.lib")

namespace {

enum class ScaleMode { Fit, Cover, Crop };

struct CropRect {
    int x = 0, y = 0, w = 0, h = 0;
    bool IsZero() const { return w <= 0 || h <= 0; }
};

ScaleMode ParseMode(const std::wstring& m)
{
    if (m == L"cover") return ScaleMode::Cover;
    if (m == L"crop") return ScaleMode::Crop;
    return ScaleMode::Fit;
}

// Перенос LoadAndScaleImage из src/StaticProducer/StaticProducer.cpp:
//   Fit   - scale by min(w/tw, h/th), letterbox on black background
//   Cover - scale by max(w/tw, h/th), center-crop to target
//   Crop  - clip settings crop rect (source pixels); stretch to target,
//           or letterbox it when keepAspect is set
bool LoadAndScaleImage(const wchar_t* filePath, ScaleMode mode, CropRect crop, bool keepAspect,
                       uint8_t* pTargetBuffer, UINT tw, UINT th, UINT tstride)
{
    CoInitialize(nullptr);

    ATL::CComPtr<IWICImagingFactory> pFactory;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&pFactory));
    if (FAILED(hr)) { pFactory = nullptr; CoUninitialize(); return false; }

    ATL::CComPtr<IWICBitmapDecoder> pDecoder;
    hr = pFactory->CreateDecoderFromFilename(filePath, nullptr, GENERIC_READ,
                                              WICDecodeMetadataCacheOnLoad, &pDecoder);
    if (FAILED(hr)) { pDecoder = nullptr; pFactory = nullptr; CoUninitialize(); return false; }

    ATL::CComPtr<IWICBitmapFrameDecode> pFrame;
    hr = pDecoder->GetFrame(0, &pFrame);
    if (FAILED(hr)) { pFrame = nullptr; pDecoder = nullptr; pFactory = nullptr; CoUninitialize(); return false; }

    UINT sw = 0, sh = 0;
    pFrame->GetSize(&sw, &sh);
    if (sw == 0 || sh == 0) {
        pFrame = nullptr; pDecoder = nullptr; pFactory = nullptr; CoUninitialize();
        return false;
    }

    ATL::CComPtr<IWICFormatConverter> pConverter;
    hr = pFactory->CreateFormatConverter(&pConverter);
    if (SUCCEEDED(hr)) {
        hr = pConverter->Initialize(pFrame, GUID_WICPixelFormat32bppBGRA,
                                    WICBitmapDitherTypeNone, nullptr, 0.0,
                                    WICBitmapPaletteTypeCustom);
    }

    IWICBitmapSource* pSource = pConverter;
    ATL::CComPtr<IWICBitmapClipper> pCropClipper;
    UINT srcW = sw, srcH = sh;

    if (SUCCEEDED(hr) && mode == ScaleMode::Crop) {
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
        nw = (UINT)llround(sw * scale);
        nh = (UINT)llround(sh * scale);
        if (mode == ScaleMode::Fit) { if (nw > tw) nw = tw; if (nh > th) nh = th; }
        else { if (nw < tw) nw = tw; if (nh < th) nh = th; }
        if (nw == 0) nw = 1;
        if (nh == 0) nh = 1;
    }

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

} // namespace

bool StaticImageSource::Open(const SourceConfig& cfg, std::wstring& err)
{
    if (open_ && cfg == cfg_) return true;
    Close();

    if (cfg.path.empty()) { err = L"empty image path"; return false; }

    std::vector<uint8_t> buf;
    try {
        buf.resize(vcam::VCamFrameSize);
    } catch (...) {
        err = L"out of memory";
        return false;
    }

    CropRect crop = { cfg.cropX, cfg.cropY, cfg.cropW, cfg.cropH };
    if (!LoadAndScaleImage(cfg.path.c_str(), ParseMode(cfg.scaleMode), crop, cfg.cropKeepAspect,
                           buf.data(), vcam::VCamWidth, vcam::VCamHeight, vcam::VCamStride)) {
        err = L"failed to load/scale image: " + cfg.path;
        return false;
    }

    frame_ = std::move(buf);
    cfg_ = cfg;
    open_ = true;
    return true;
}

bool StaticImageSource::Render(uint8_t* bgrx, int stride, std::wstring& err)
{
    if (!open_) { err = L"static source is not open"; return false; }
    if (!bgrx || stride < (int)vcam::VCamStride) { err = L"invalid target buffer"; return false; }

    if (stride == (int)vcam::VCamStride) {
        memcpy(bgrx, frame_.data(), vcam::VCamFrameSize);
    } else {
        for (UINT32 y = 0; y < vcam::VCamHeight; y++) {
            memcpy(bgrx + (SIZE_T)y * (size_t)stride,
                   frame_.data() + (SIZE_T)y * vcam::VCamStride, vcam::VCamStride);
        }
    }
    return true;
}

void StaticImageSource::Close()
{
    frame_.clear();
    frame_.shrink_to_fit();
    cfg_ = SourceConfig();
    open_ = false;
}
