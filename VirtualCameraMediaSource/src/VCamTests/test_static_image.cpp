#include "doctest.h"
#include <windows.h>
#include <wincodec.h>
#include <atlbase.h>

#include "ProducerApi.h"
#include "StaticImageSource.h"

#include <string>
#include <vector>

#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")

// Регресс-тест фикса 07.10.2026: в режиме crop NativeSize обязан отдавать
// пропорции crop-rect'а, а не натива источника. Раньше портретный натив +
// горизонтальный crop рендерились в портретный буфер: crop растягивался,
// а v1 (letterbox в 720p) получал чёрные пилларбоксы по бокам.

namespace {

std::wstring TempPath(const wchar_t* name)
{
    wchar_t tmp[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, tmp);
    return std::wstring(tmp) + name;
}

// Сплошной 32bpp PNG через WIC (тестовый «портретный источник»).
bool MakeSolidPng(const std::wstring& path, UINT w, UINT h, BYTE b, BYTE g, BYTE r)
{
    CoInitialize(nullptr);
    bool ok = false;
    ATL::CComPtr<IWICImagingFactory> f;
    if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARGS(&f)))) {
        IWICBitmap* bmp = nullptr;
        if (SUCCEEDED(f->CreateBitmap(w, h, GUID_WICPixelFormat32bppBGRA,
                                      WICBitmapCacheOnLoad, &bmp)) && bmp) {
            std::vector<BYTE> px((size_t)w * h * 4);
            for (size_t i = 0; i < px.size(); i += 4) {
                px[i] = b;
                px[i + 1] = g;
                px[i + 2] = r;
                px[i + 3] = 255;
            }
            WICRect rc = { 0, 0, (INT)w, (INT)h };
            IWICBitmapLock* lk = nullptr;
            if (SUCCEEDED(bmp->Lock(&rc, WICBitmapLockWrite, &lk)) && lk) {
                BYTE* data = nullptr;
                UINT cap = 0;
                if (SUCCEEDED(lk->GetDataPointer(&cap, &data)) && data &&
                    cap >= px.size()) {
                    memcpy(data, px.data(), px.size());
                }
                lk->Release();
                lk = nullptr;
                ATL::CComPtr<IWICStream> st;
                if (SUCCEEDED(f->CreateStream(&st)) &&
                    SUCCEEDED(st->InitializeFromFilename(path.c_str(), GENERIC_WRITE))) {
                    ATL::CComPtr<IWICBitmapEncoder> enc;
                    if (SUCCEEDED(f->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) &&
                        SUCCEEDED(enc->Initialize(st, WICBitmapEncoderNoCache))) {
                        ATL::CComPtr<IWICBitmapFrameEncode> frame;
                        if (SUCCEEDED(enc->CreateNewFrame(&frame, nullptr)) &&
                            SUCCEEDED(frame->Initialize(nullptr)) &&
                            SUCCEEDED(frame->WriteSource(bmp, nullptr)) &&
                            SUCCEEDED(frame->Commit()) && SUCCEEDED(enc->Commit())) {
                            ok = true;
                        }
                    }
                }
            }
            bmp->Release();
        }
    }
    f = nullptr; // COM-объекты освободить ДО CoUninitialize (иначе деструктор крашит)
    CoUninitialize();
    return ok;
}

BYTE PixelAt(const std::vector<uint8_t>& buf, int stride, int x, int y, int channel)
{
    return buf[(size_t)y * stride + x * 4 + channel];
}

} // namespace

TEST_CASE("static crop: NativeSize reports crop-rect size, not native")
{
    const UINT sw = 1000, sh = 2000; // портретный источник
    auto path = TempPath(L"vcam_test_crop_native.png");
    REQUIRE(MakeSolidPng(path, sw, sh, 200, 100, 50)); // BGR сплошной

    SourceConfig cfg;
    cfg.type = L"static";
    cfg.path = path;
    cfg.scaleMode = L"crop";
    cfg.cropX = 0;
    cfg.cropY = 500;
    cfg.cropW = 1000;
    cfg.cropH = 562; // ≈16:9 горизонтальная полоса
    cfg.cropKeepAspect = false;

    StaticImageSource src;
    std::wstring err;
    REQUIRE(src.Open(cfg, err));

    uint32_t w = 0, h = 0;
    CHECK(src.NativeSize(w, h));
    CHECK(w == 1000); // был натив 1000x2000 -> crop рендерился в портрет
    CHECK(h == 562);

    // Рендер в заявленный размер: сплошной цвет во всех углах = полос нет.
    std::vector<uint8_t> buf((size_t)w * h * 4, 0);
    REQUIRE(src.Render(buf.data(), (int)(w * 4), w, h, err));
    for (int corner = 0; corner < 4; ++corner) {
        int x = corner % 2 ? (int)w - 1 : 0;
        int y = corner / 2 ? (int)h - 1 : 0;
        CHECK(PixelAt(buf, (int)(w * 4), x, y, 0) == 200);
        CHECK(PixelAt(buf, (int)(w * 4), x, y, 1) == 100);
        CHECK(PixelAt(buf, (int)(w * 4), x, y, 2) == 50);
    }

    src.Close();
    DeleteFileW(path.c_str());
}

TEST_CASE("static fit: NativeSize still reports native")
{
    const UINT sw = 1000, sh = 2000;
    auto path = TempPath(L"vcam_test_fit_native.png");
    REQUIRE(MakeSolidPng(path, sw, sh, 200, 100, 50));

    SourceConfig cfg;
    cfg.type = L"static";
    cfg.path = path;
    cfg.scaleMode = L"fit";

    StaticImageSource src;
    std::wstring err;
    REQUIRE(src.Open(cfg, err));

    uint32_t w = 0, h = 0;
    CHECK(src.NativeSize(w, h));
    CHECK(w == sw);
    CHECK(h == sh);

    src.Close();
    DeleteFileW(path.c_str());
}

// IFrameSource play/pause defaults: не-video источники игнорируют команду
// хоткея (PlayPauseToggle no-op) и не заявляют паузу/конец. Ломается дефолт —
// хост получит мусор при вызове ToggleVideoPlay на static/camera.
TEST_CASE("framesource: play/pause defaults are no-ops for static")
{
    const UINT sw = 64, sh = 64;
    auto path = TempPath(L"vcam_test_toggle_defaults.png");
    REQUIRE(MakeSolidPng(path, sw, sh, 10, 20, 30));

    SourceConfig cfg;
    cfg.type = L"static";
    cfg.path = path;

    StaticImageSource src;
    std::wstring err;
    REQUIRE(src.Open(cfg, err));

    CHECK(src.IsPaused() == false);
    CHECK(src.Ended() == false);
    src.PlayPauseToggle(); // не должно менять состояние и не должно падать
    CHECK(src.IsPaused() == false);
    CHECK(src.Ended() == false);

    uint32_t w = 0, h = 0;
    CHECK(src.NativeSize(w, h)); // источник по-прежнему рабочий

    src.Close();
    DeleteFileW(path.c_str());
}

