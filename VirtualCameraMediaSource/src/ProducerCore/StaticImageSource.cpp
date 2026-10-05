#include "StaticImageSource.h"

#include <wincodec.h>
#include <atlbase.h>

#include <cmath>
#include <cstring>

#include "FrameCopy.h"
#include "ImageLayout.h"
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

// Нативный декод (WIC, 1:1; больше cap — fit-даунскейл HighQualityCubic).
// cropScale отдают отношение натив/исходник (crop-rect задан в исх. пикселях).
bool DecodeNativeImage(const wchar_t* filePath, std::vector<uint8_t>& pixels,
                       UINT& w, UINT& h, double& scaleX, double& scaleY)
{
    pixels.clear();
    w = h = 0;
    scaleX = scaleY = 1.0;
    CoInitialize(nullptr);

    ATL::CComPtr<IWICImagingFactory> pFactory;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&pFactory));
    bool ok = false;
    if (SUCCEEDED(hr) && pFactory) {
        ATL::CComPtr<IWICBitmapDecoder> pDecoder;
        hr = pFactory->CreateDecoderFromFilename(filePath, nullptr, GENERIC_READ,
                                                 WICDecodeMetadataCacheOnLoad, &pDecoder);
        if (SUCCEEDED(hr) && pDecoder) {
            ATL::CComPtr<IWICBitmapFrameDecode> pFrame;
            hr = pDecoder->GetFrame(0, &pFrame);
            UINT sw = 0, sh = 0;
            if (SUCCEEDED(hr) && pFrame) pFrame->GetSize(&sw, &sh);
            if (sw != 0 && sh != 0) {
                ATL::CComPtr<IWICFormatConverter> pConverter;
                hr = pFactory->CreateFormatConverter(&pConverter);
                if (SUCCEEDED(hr)) {
                    hr = pConverter->Initialize(pFrame, GUID_WICPixelFormat32bppBGRA,
                                                WICBitmapDitherTypeNone, nullptr, 0.0,
                                                WICBitmapPaletteTypeCustom);
                }
                IWICBitmapSource* pSource = pConverter;
                ATL::CComPtr<IWICBitmapScaler> pScaler;
                UINT nw = sw, nh = sh;
                if (SUCCEEDED(hr) && pSource &&
                    (sw > vcam::VCamNativeCapW || sh > vcam::VCamNativeCapH)) {
                    double s = (double)vcam::VCamNativeCapW / sw;
                    double s2 = (double)vcam::VCamNativeCapH / sh;
                    if (s2 < s) s = s2;
                    nw = (UINT)llround(sw * s);
                    nh = (UINT)llround(sh * s);
                    if (nw < 1) nw = 1;
                    if (nh < 1) nh = 1;
                    if (nw > vcam::VCamNativeCapW) nw = vcam::VCamNativeCapW;
                    if (nh > vcam::VCamNativeCapH) nh = vcam::VCamNativeCapH;
                    hr = pFactory->CreateBitmapScaler(&pScaler);
                    if (SUCCEEDED(hr)) {
                        hr = pScaler->Initialize(pSource, nw, nh,
                                                 WICBitmapInterpolationModeHighQualityCubic);
                        if (SUCCEEDED(hr)) pSource = pScaler;
                    }
                }
                if (SUCCEEDED(hr) && pSource) {
                    std::vector<uint8_t> tmp((size_t)nw * nh * 4);
                    if (SUCCEEDED(pSource->CopyPixels(nullptr, nw * 4, (UINT)tmp.size(),
                                                     tmp.data()))) {
                        pixels = std::move(tmp);
                        w = nw;
                        h = nh;
                        scaleX = (double)nw / sw;
                        scaleY = (double)nh / sh;
                        ok = true;
                    }
                }
                pScaler = nullptr;
                pConverter = nullptr;
            }
            pFrame = nullptr;
        }
        pDecoder = nullptr;
        pFactory = nullptr;
    }
    CoUninitialize();
    return ok;
}

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
    // Натив — best effort: не взлетел — NativeSize=false, sized-рендер
    // фолбэчит с 720p-кэша (v1-путь выше при этом цел).
    native_.clear();
    nativeW_ = nativeH_ = 0;
    cropScaleX_ = cropScaleY_ = 1.0;
    UINT nw = 0, nh = 0;
    double sx = 1.0, sy = 1.0;
    if (DecodeNativeImage(cfg.path.c_str(), native_, nw, nh, sx, sy) && !native_.empty()) {
        nativeW_ = nw;
        nativeH_ = nh;
        cropScaleX_ = sx;
        cropScaleY_ = sy;
    }
    cfg_ = cfg;
    open_ = true;
    return true;
}

bool StaticImageSource::Render(uint8_t* bgrx, int stride, std::wstring& err)
{
    if (!open_) { err = L"static source is not open"; return false; }
    if (!bgrx || stride < (int)vcam::VCamStride) { err = L"invalid target buffer"; return false; }

    vcam::CopyFrameRowwise(bgrx, (size_t)stride, frame_.data(), vcam::VCamStride,
                           vcam::VCamWidth, vcam::VCamHeight, vcam::VCamPixelSize);
    return true;
}

bool StaticImageSource::NativeSize(uint32_t& w, uint32_t& h)
{
    if (!open_ || native_.empty() || nativeW_ == 0 || nativeH_ == 0) {
        w = vcam::VCamWidth;
        h = vcam::VCamHeight;
        return false;
    }
    w = nativeW_;
    h = nativeH_;
    return true;
}

bool StaticImageSource::Render(uint8_t* dst, int stride, uint32_t w, uint32_t h,
                               std::wstring& err)
{
    if (!open_) { err = L"static source is not open"; return false; }
    if (!dst || w == 0 || h == 0 || w > vcam::VCamNativeCapW || h > vcam::VCamNativeCapH ||
        stride < (int)(w * 4)) {
        err = L"invalid target buffer";
        return false;
    }

    // Деградация (натив не декодировался): fit с 720p-кэша.
    const uint8_t* base = native_.empty() ? frame_.data() : native_.data();
    UINT sw = native_.empty() ? vcam::VCamWidth : nativeW_;
    UINT sh = native_.empty() ? vcam::VCamHeight : nativeH_;
    LONG sstride = (LONG)(sw * 4);

    // Быстрый путь «размер-в-размер»: корректен только для Fit/Cover
    // (цель уже равна исходнику, обрабатывать нечего). Для Crop через него
    // проходить нельзя — рект был бы проигнорирован и в эфир ушёл бы полный
    // кадр (баг 05.10.2026: crop в эфире не применялся).
    ScaleMode mode = ParseMode(cfg_.scaleMode);
    if (mode != ScaleMode::Crop && !native_.empty() && w == nativeW_ && h == nativeH_) {
        vcam::CopyFrameRowwise(dst, (size_t)stride, native_.data(), (size_t)sstride,
                               nativeW_, nativeH_, vcam::VCamPixelSize);
        return true;
    }

    if (native_.empty() || mode == ScaleMode::Fit) {
        vcam::LetterboxBilinearEx(base, sw, sh, sstride, dst, w, h, (LONG)stride);
        return true;
    }
    if (mode == ScaleMode::Cover) {
        vcam::CoverBilinearEx(base, sw, sh, sstride, dst, w, h, (LONG)stride);
        return true;
    }
    // Crop: rect из конфига (исходные пиксели) -> в нативные координаты.
    int rx = (int)llround(cfg_.cropX * cropScaleX_);
    int ry = (int)llround(cfg_.cropY * cropScaleY_);
    int rw = (int)llround(cfg_.cropW * cropScaleX_);
    int rh = (int)llround(cfg_.cropH * cropScaleY_);
    if (rx < 0) rx = 0;
    if (ry < 0) ry = 0;
    if (rx >= (int)sw) rx = 0;
    if (ry >= (int)sh) ry = 0;
    if (rw <= 0 || rw > (int)sw - rx) rw = (int)sw - rx;
    if (rh <= 0 || rh > (int)sh - ry) rh = (int)sh - ry;
    if (rw <= 0 || rh <= 0) { // пустой rect после клампа — как fit
        vcam::LetterboxBilinearEx(base, sw, sh, sstride, dst, w, h, (LONG)stride);
        return true;
    }
    const BYTE* rect = base + (size_t)ry * sstride + (size_t)rx * 4;
    if (cfg_.cropKeepAspect)
        vcam::LetterboxBilinearEx(rect, (UINT)rw, (UINT)rh, sstride, dst, w, h, (LONG)stride);
    else
        vcam::StretchBilinearEx(rect, (UINT)rw, (UINT)rh, sstride, dst, w, h, (LONG)stride);
    return true;
}

void StaticImageSource::Close()
{
    frame_.clear();
    frame_.shrink_to_fit();
    native_.clear();
    native_.shrink_to_fit();
    nativeW_ = nativeH_ = 0;
    cropScaleX_ = cropScaleY_ = 1.0;
    cfg_ = SourceConfig();
    open_ = false;
}
