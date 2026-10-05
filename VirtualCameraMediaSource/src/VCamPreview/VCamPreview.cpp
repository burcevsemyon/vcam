#include <windows.h>
#include <windowsx.h>
#include <gdiplus.h>
#include <atlbase.h>
#include <cstdio>
#include <cwchar>
#include <cstring>
#include <memory>
#include <new>
#include "../Common/SharedMemoryContract.h"
#include "../Common/MappedViewOfFilePtr.h"

#pragma comment(lib, "gdiplus.lib")

namespace {

constexpr UINT kDefaultWidth = 640;
constexpr UINT kDefaultHeight = 360;
constexpr UINT kTimerId = 1;
constexpr UINT kTimerIntervalMs = 33;   // ~30 FPS repaint
constexpr UINT64 kConnectRetryMs = 1000;
constexpr UINT64 kSignalTimeoutMs = 1500;   // seq stopped changing -> no signal
constexpr UINT64 kReconnectAfterMs = 3000;  // stale mapping -> drop and reopen
constexpr UINT32 kMaxDimension = 4096;
constexpr UINT64 kMaxFrameBytes = 64ull * 1024 * 1024;
constexpr int kResizeMargin = 8;
constexpr const wchar_t* kWindowTitle = L"VCam Preview";
constexpr const wchar_t* kWndClass = L"VCamPreviewClass";
constexpr const wchar_t* kInstanceMutexName = L"VCamPreview.Instance";

struct PreviewState {
    // shared memory reader
    ATL::CHandle hSection;
    vcam::MappedViewOfFilePtr view;
    SIZE_T cbMapped = 0;
    vcam::VCamSectionHeader* pHeader = nullptr;
    bool connected = false;
    UINT64 lastConnectTry = 0;
    bool isV2 = false;       // v2-секция: слоты фиксированного питча MaxFrameSize
    SIZE_T slotPitch = 0;    // байт на слот (v2: VCamV2MaxFrameSize, v1: frameSize)

    // frame staging (tightly packed rows: stride = width * 4)
    std::unique_ptr<BYTE[]> pFrame;
    UINT32 frameW = 0, frameH = 0;
    UINT32 allocBytes = 0;

    // seqlock / liveness
    LONGLONG lastSeq = 0;
    UINT64 lastSeqChange = 0;
    UINT64 nextReconnectAt = 0;
    bool haveFrame = false;
    bool alive = false;

    // title state (update only on change)
    bool titleAlive = false;
    UINT32 titleW = 0, titleH = 0;

    // GDI double buffer
    HDC memDC = nullptr;
    HBITMAP memBmp = nullptr;
    HBITMAP memOldBmp = nullptr;
    int memW = 0, memH = 0;
    HFONT font = nullptr;
};

PreviewState g;

ULONG_PTR gGdiplusToken = 0;

bool InitGdiplus()
{
    Gdiplus::GdiplusStartupInput input;
    return Gdiplus::GdiplusStartup(&gGdiplusToken, &input, nullptr) == Gdiplus::Ok;
}

void ShutdownGdiplus()
{
    if (gGdiplusToken) {
        Gdiplus::GdiplusShutdown(gGdiplusToken);
        gGdiplusToken = 0;
    }
}

UINT64 NowMs() { return GetTickCount64(); }

void ReleaseFrameBuffer()
{
    g.pFrame.reset();
    g.allocBytes = 0;
    g.frameW = g.frameH = 0;
    g.haveFrame = false;
}

void Disconnect()
{
    ReleaseFrameBuffer();
    g.view.Close();
    g.hSection.Close();
    g.pHeader = nullptr;
    g.cbMapped = 0;
    g.isV2 = false;
    g.slotPitch = 0;
    g.connected = false;
    g.alive = false;
    // Keep lastSeq/lastSeqChange: reconnect must see that the old frame is stale
    // instead of treating it as fresh (that caused flicker after provider stop).
}

bool ValidateHeader(const vcam::VCamSectionHeader* h)
{
    if (h->magic != vcam::VCamMagic) return false;
    if (h->version != vcam::VCamVersion) return false;
    if (h->width == 0 || h->width > kMaxDimension) return false;
    if (h->height == 0 || h->height > kMaxDimension) return false;
    if (h->stride < h->width * vcam::VCamPixelSize || h->stride > 1024 * 1024) return false;
    if (h->pixelFormat != (UINT32)vcam::VCamPixelFormat::RGB32) return false;
    if (h->frameSize < (UINT64)h->stride * h->height || h->frameSize > kMaxFrameBytes) return false;
    if (h->slotCount == 0 || h->slotCount > 64) return false;
    if ((UINT64)sizeof(vcam::VCamSectionHeader) + (UINT64)h->slotCount * h->frameSize > kMaxFrameBytes) return false;
    return true;
}

// v2: диметры динамические (до cap 4K), слоты фиксированного питча
// VCamV2MaxFrameSize. Маппинг всей секции ~126.6 МБ — отдельный лимит.
constexpr UINT64 kMaxV2MappedBytes = 140ull * 1024 * 1024;

bool ValidateHeaderV2(const vcam::VCamSectionHeader* h)
{
    if (h->magic != vcam::VCamMagic) return false;
    if (h->version != vcam::VCamVersionV2) return false;
    if (h->pixelFormat != (UINT32)vcam::VCamPixelFormat::RGB32) return false;
    if (h->width == 0 || h->width > vcam::VCamV2MaxWidth) return false;
    if (h->height == 0 || h->height > vcam::VCamV2MaxHeight) return false;
    if (h->stride < h->width * vcam::VCamPixelSize || h->stride > vcam::VCamV2MaxStride) return false;
    if (h->frameSize < (UINT64)h->stride * h->height ||
        (UINT64)h->frameSize > vcam::VCamV2MaxFrameSize) return false;
    if (h->slotCount == 0 || h->slotCount > vcam::VCamV2SlotCount) return false;
    if ((UINT64)sizeof(vcam::VCamSectionHeader) +
        (UINT64)h->slotCount * vcam::VCamV2MaxFrameSize > kMaxV2MappedBytes) return false;
    return true;
}

bool MappedBoundOk()
{
    if (!g.pHeader) return false;
    if (g.isV2) {
        return (UINT64)sizeof(vcam::VCamSectionHeader) +
               (UINT64)g.pHeader->slotCount * vcam::VCamV2MaxFrameSize <= g.cbMapped;
    }
    return (UINT64)sizeof(vcam::VCamSectionHeader) +
           (UINT64)g.pHeader->slotCount * g.pHeader->frameSize <= g.cbMapped;
}

bool TryConnect(UINT64 now)
{
    if (g.connected) return true;
    if (now - g.lastConnectTry < kConnectRetryMs) return false;
    g.lastConnectTry = now;

    // v2 вперёд (натив, резче), затем v1 как раньше. Только чтение
    // (FILE_MAP_READ), писателя не открываем. Мусорная v2 — не приговор:
    // идём дальше по кандидатам (фолбэк на v1).
    struct Cand { const wchar_t* name; bool v2; };
    const Cand cands[] = {
        { vcam::VCamSectionNameV2, true },
        { L"Local\\VCam.FrameBuffer.v2", true },
        { vcam::VCamSectionName, false },
        { L"Local\\VCam.FrameBuffer.v1", false },
    };
    ATL::CHandle hSection;
    vcam::MappedViewOfFilePtr view;
    SIZE_T cbMapped = 0;
    vcam::VCamSectionHeader* pHeader = nullptr;
    bool candV2 = false;
    SIZE_T slotPitch = 0;
    for (const Cand& c : cands) {
        hSection.Attach(OpenFileMappingW(FILE_MAP_READ, FALSE, c.name));
        if (!hSection) {
            DWORD err = GetLastError();
            if (err != ERROR_FILE_NOT_FOUND && err != ERROR_ACCESS_DENIED) break;
            continue;
        }
        BYTE* pRaw = (BYTE*)MapViewOfFile(hSection, FILE_MAP_READ, 0, 0, 0);
        vcam::MappedViewOfFilePtr v(pRaw);
        if (!v) { hSection.Close(); continue; }
        BYTE* pBase = (BYTE*)v.Get();
        MEMORY_BASIC_INFORMATION mbi = {};
        SIZE_T cb = 0;
        if (VirtualQuery(pBase, &mbi, sizeof(mbi)) != 0) cb = mbi.RegionSize;
        vcam::VCamSectionHeader* h =
            reinterpret_cast<vcam::VCamSectionHeader*>(pBase);
        SIZE_T pitch = 0;
        bool ok = (cb >= sizeof(vcam::VCamSectionHeader));
        if (ok && c.v2) {
            ok = ValidateHeaderV2(h);
            pitch = (SIZE_T)vcam::VCamV2MaxFrameSize;
        } else if (ok) {
            ok = ValidateHeader(h);
            pitch = h->frameSize;
        }
        if (ok) {
            ok = ((UINT64)sizeof(vcam::VCamSectionHeader) +
                  (UINT64)h->slotCount * pitch <= cb);
        }
        if (!ok) { hSection.Close(); continue; } // мусор — следующий кандидат
        view.Attach(v.Detach());
        cbMapped = cb;
        pHeader = h;
        candV2 = c.v2;
        slotPitch = pitch;
        break;
    }
    if (!hSection || !view || !pHeader || slotPitch == 0) return false;

    UINT32 frameBytes = pHeader->stride * pHeader->height;
    if (frameBytes == 0 || frameBytes > kMaxFrameBytes) return false;
    std::unique_ptr<BYTE[]> pFrame;
    try { pFrame = std::make_unique_for_overwrite<BYTE[]>(frameBytes); }
    catch (const std::bad_alloc&) { pFrame.reset(); }
    if (!pFrame) {
        return false;
    }

    g.hSection.Attach(hSection.Detach());
    g.view.Attach(view.Detach());
    g.cbMapped = cbMapped;
    g.isV2 = candV2;
    g.slotPitch = slotPitch;
    g.pHeader = pHeader;
    g.pFrame = std::move(pFrame);
    g.frameW = pHeader->width;
    g.frameH = pHeader->height;
    g.allocBytes = frameBytes;
    g.connected = true;
    g.haveFrame = false;
    LONGLONG seqNow = pHeader->seq;
    if (seqNow != g.lastSeq) g.lastSeqChange = now;  // only a NEW frame refreshes liveness
    g.lastSeq = seqNow;
    g.nextReconnectAt = now + kReconnectAfterMs;
    return true;
}

// Seqlock read into tightly packed staging buffer.
// Returns true when a consistent frame (old seq even, unchanged across the copy)
// has been copied. seq is reported through outSeq for liveness tracking.
bool ReadFrame(LONGLONG* outSeq)
{
    if (!g.connected || !g.view || !g.pHeader || !g.pFrame) return false;

    for (int spin = 0; spin < 4096; ++spin) {
        LONGLONG seq = g.pHeader->seq;
        if (seq & 1) { YieldProcessor(); continue; }

        UINT32 idx = g.pHeader->frameWriteIndex;
        if (idx >= g.pHeader->slotCount) return false;
        if (g.isV2 ? !ValidateHeaderV2(g.pHeader) : !ValidateHeader(g.pHeader)) return false;

        LONGLONG seq2 = g.pHeader->seq;
        if (seq != seq2) continue;

        UINT32 w = g.pHeader->width;
        UINT32 h = g.pHeader->height;
        UINT32 srcStride = g.pHeader->stride;
        UINT32 dstStride = w * vcam::VCamPixelSize;

        if (w != g.frameW || h != g.frameH || (UINT64)srcStride * h > g.allocBytes) {
            UINT32 need = srcStride * h;
            if (need > kMaxFrameBytes) return false;
            std::unique_ptr<BYTE[]> pNew;
            try { pNew = std::make_unique_for_overwrite<BYTE[]>(need); }
            catch (const std::bad_alloc&) { pNew.reset(); }
            if (!pNew) return false;
            g.pFrame = std::move(pNew);
            g.allocBytes = need;
            g.frameW = w;
            g.frameH = h;
        }

        const BYTE* pSrc = (const BYTE*)g.view.Get() + sizeof(vcam::VCamSectionHeader) + (SIZE_T)idx * g.slotPitch;
        if (dstStride != srcStride) {
            for (UINT32 y = 0; y < h; ++y)
                memcpy(g.pFrame.get() + (SIZE_T)y * dstStride, pSrc + (SIZE_T)y * srcStride, dstStride);
        } else {
            memcpy(g.pFrame.get(), pSrc, (SIZE_T)dstStride * h);
        }

        LONGLONG seq3 = g.pHeader->seq;
        if (seq3 != seq) continue;

        g.haveFrame = true;
        *outSeq = seq;
        return true;
    }
    return false;
}

void SetTitle(HWND hwnd, bool alive, UINT32 w, UINT32 h)
{
    if (alive == g.titleAlive && w == g.titleW && h == g.titleH) return;
    g.titleAlive = alive;
    g.titleW = w;
    g.titleH = h;

    wchar_t buf[128];
    if (alive) {
        // Nominal camera rate from the contract (VCamFrameInterval100ns = 333333 -> 30 fps).
        const UINT fps = (UINT)((10000000ull + vcam::VCamFrameInterval100ns / 2) / vcam::VCamFrameInterval100ns);
        swprintf_s(buf, L"%s — %ux%u@%u", kWindowTitle, w, h, fps);
    } else {
        swprintf_s(buf, L"%s — нет сигнала", kWindowTitle);
    }
    SetWindowTextW(hwnd, buf);
}

void Tick(HWND hwnd)
{
    UINT64 now = NowMs();

    if (!g.connected) {
        TryConnect(now);
    }

    if (g.connected) {
        if ((g.isV2 ? !ValidateHeaderV2(g.pHeader) : !ValidateHeader(g.pHeader)) || !MappedBoundOk()) {
            Disconnect();
        } else {
            LONGLONG seq = 0;
            if (ReadFrame(&seq) && seq != g.lastSeq) {
                g.lastSeq = seq;
                g.lastSeqChange = now;
            }
            bool alive = g.haveFrame && (now - g.lastSeqChange) <= kSignalTimeoutMs;
            if (!alive && now >= g.nextReconnectAt) {
                // Provider is gone; our handle keeps the dead mapping alive.
                // Drop it so the retry below can open a fresh section.
                // Rate-limited by nextReconnectAt so a stale frame is not
                // re-announced as fresh on every retry (flicker on stop).
                Disconnect();
            } else {
                g.alive = alive;
            }
        }
    }

    SetTitle(hwnd, g.connected && g.alive, g.frameW, g.frameH);

    // A foreign window can steal topmost; re-assert cheaply.
    if (!(GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST)) {
        SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }

    InvalidateRect(hwnd, nullptr, FALSE);
}

void EnsureMemBuffer(HDC hdc, int w, int h)
{
    if (g.memDC && g.memW == w && g.memH == h) return;
    if (g.memDC) {
        SelectObject(g.memDC, g.memOldBmp);
        DeleteObject(g.memBmp);
        DeleteDC(g.memDC);
        g.memDC = nullptr;
        g.memBmp = nullptr;
        g.memOldBmp = nullptr;
    }
    if (w <= 0 || h <= 0) return;

    HDC memDC = CreateCompatibleDC(hdc);
    if (!memDC) return;
    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h; // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    HBITMAP bmp = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, nullptr, nullptr, 0);
    if (!bmp) { DeleteDC(memDC); return; }
    HBITMAP old = (HBITMAP)SelectObject(memDC, bmp);
    g.memDC = memDC;
    g.memBmp = bmp;
    g.memOldBmp = old;
    g.memW = w;
    g.memH = h;
}

void PaintFrame(HDC memDC, int cw, int ch)
{
    double scale = (double)cw / g.frameW;
    if ((double)ch / g.frameH < scale) scale = (double)ch / g.frameH;
    int dw = (int)(g.frameW * scale);
    int dh = (int)(g.frameH * scale);
    if (dw < 1) dw = 1;
    if (dh < 1) dh = 1;
    int dx = (cw - dw) / 2;
    int dy = (ch - dh) / 2;

    // g.pFrame is a top-down tightly packed BGRX staging buffer, filled by
    // ReadFrame() on this same UI thread, so it is stable for the draw call.
    Gdiplus::Bitmap src((INT)g.frameW, (INT)g.frameH, (INT)(g.frameW * 4),
                        PixelFormat32bppRGB, g.pFrame.get());
    if (src.GetLastStatus() != Gdiplus::Ok) return;

    Gdiplus::Graphics gfx(memDC);
    if (gfx.GetLastStatus() != Gdiplus::Ok) return;
    gfx.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
    gfx.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
    gfx.SetCompositingQuality(Gdiplus::CompositingQualityHighQuality);
    gfx.SetCompositingMode(Gdiplus::CompositingModeSourceCopy);
    gfx.DrawImage(&src, Gdiplus::Rect(dx, dy, dw, dh),
                  0, 0, (INT)g.frameW, (INT)g.frameH, Gdiplus::UnitPixel);
}

void PaintNoSignal(HDC memDC, int cw, int ch)
{
    SetBkMode(memDC, TRANSPARENT);
    SetTextColor(memDC, RGB(235, 235, 235));
    HFONT oldFont = g.font ? (HFONT)SelectObject(memDC, g.font) : nullptr;
    RECT rc = { 0, 0, cw, ch };
    DrawTextW(memDC, L"NO SIGNAL", -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    if (oldFont) SelectObject(memDC, oldFont);
}

void OnPaint(HWND hwnd)
{
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);
    RECT rc;
    GetClientRect(hwnd, &rc);
    int cw = rc.right, ch = rc.bottom;

    if (cw > 0 && ch > 0) {
        EnsureMemBuffer(hdc, cw, ch);
        if (g.memDC) {
            PatBlt(g.memDC, 0, 0, cw, ch, BLACKNESS);
            if (g.connected && g.alive && g.haveFrame && g.pFrame) {
                PaintFrame(g.memDC, cw, ch);
            } else {
                PaintNoSignal(g.memDC, cw, ch);
            }
            BitBlt(hdc, 0, 0, cw, ch, g.memDC, 0, 0, SRCCOPY);
        }
    }
    EndPaint(hwnd, &ps);
}

void ResetWindowSize(HWND hwnd)
{
    RECT rc = { 0, 0, (LONG)kDefaultWidth, (LONG)kDefaultHeight };
    AdjustWindowRectEx(&rc, (DWORD)GetWindowLongPtrW(hwnd, GWL_STYLE), FALSE,
                       (DWORD)GetWindowLongPtrW(hwnd, GWL_EXSTYLE));
    SetWindowPos(hwnd, nullptr, 0, 0, rc.right - rc.left, rc.bottom - rc.top,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_CREATE:
        SetTimer(hwnd, kTimerId, kTimerIntervalMs, nullptr);
        return 0;

    case WM_TIMER:
        if (wParam == kTimerId) { Tick(hwnd); return 0; }
        break;

    case WM_PAINT:
        OnPaint(hwnd);
        return 0;

    case WM_ERASEBKGND:
        return 1; // painted by the double buffer

    case WM_NCHITTEST: {
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        RECT wr;
        GetWindowRect(hwnd, &wr);
        const bool left = pt.x < wr.left + kResizeMargin;
        const bool right = pt.x >= wr.right - kResizeMargin;
        const bool top = pt.y < wr.top + kResizeMargin;
        const bool bottom = pt.y >= wr.bottom - kResizeMargin;
        if (left && top) return HTTOPLEFT;
        if (right && top) return HTTOPRIGHT;
        if (left && bottom) return HTBOTTOMLEFT;
        if (right && bottom) return HTBOTTOMRIGHT;
        if (left) return HTLEFT;
        if (right) return HTRIGHT;
        if (top) return HTTOP;
        if (bottom) return HTBOTTOM;
        return HTCAPTION; // drag by the client area
    }

    case WM_NCLBUTTONDBLCLK:
        ResetWindowSize(hwnd); // double-click resets to 640x360
        return 0;

    case WM_GETMINMAXINFO: {
        MINMAXINFO* mmi = (MINMAXINFO*)lParam;
        mmi->ptMinTrackSize.x = 160;
        mmi->ptMinTrackSize.y = 90;
        return 0;
    }

    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) { DestroyWindow(hwnd); return 0; }
        if (wParam == 'Q' && (GetKeyState(VK_CONTROL) & 0x8000)) { DestroyWindow(hwnd); return 0; }
        return 0;

    case WM_DESTROY:
        KillTimer(hwnd, kTimerId);
        Disconnect();
        if (g.memDC) {
            SelectObject(g.memDC, g.memOldBmp);
            DeleteObject(g.memBmp);
            DeleteDC(g.memDC);
            g.memDC = nullptr;
            g.memBmp = nullptr;
        }
        if (g.font) { DeleteObject(g.font); g.font = nullptr; }
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void PlaceTopRight(HWND hwnd)
{
    POINT pt = {};
    GetCursorPos(&pt);
    HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = { sizeof(mi) };
    if (!GetMonitorInfoW(mon, &mi)) {
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &mi.rcWork, 0);
    }
    RECT wr;
    GetWindowRect(hwnd, &wr);
    int w = wr.right - wr.left, h = wr.bottom - wr.top;
    int x = mi.rcWork.right - w - 16;
    int y = mi.rcWork.top + 16;
    if (x < mi.rcWork.left) x = mi.rcWork.left;
    if (y < mi.rcWork.top) y = mi.rcWork.top;
    SetWindowPos(hwnd, HWND_TOPMOST, x, y, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
}

} // namespace

namespace {

void ActivateExistingInstance()
{
    HWND existing = FindWindowW(kWndClass, nullptr);
    if (!existing) return;

    const DWORD curTid = GetCurrentThreadId();
    HWND fg = GetForegroundWindow();
    DWORD fgTid = fg ? GetWindowThreadProcessId(fg, nullptr) : 0;
    const bool attached = (fgTid != 0 && fgTid != curTid && AttachThreadInput(curTid, fgTid, TRUE));

    if (IsIconic(existing)) SendNotifyMessageW(existing, WM_SYSCOMMAND, SC_RESTORE, 0);
    ShowWindow(existing, SW_RESTORE);
    BringWindowToTop(existing);
    SetForegroundWindow(existing);

    if (attached) AttachThreadInput(curTid, fgTid, FALSE);

    if (GetForegroundWindow() != existing) {
        SetWindowPos(existing, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
        SetForegroundWindow(existing);
    }
}

} // namespace

int APIENTRY wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int nCmdShow)
{
    ATL::CHandle hInstanceMutex(CreateMutexW(nullptr, TRUE, kInstanceMutexName));
    if (!hInstanceMutex) return 1;
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        ActivateExistingInstance();
        return 0;
    }

    if (!InitGdiplus()) {
        return 1;
    }

    WNDCLASSEXW wc = { sizeof(wc) };
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = kWndClass;
    if (!RegisterClassExW(&wc)) {
        ShutdownGdiplus();
        return 1;
    }

    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME;
    RECT rc = { 0, 0, (LONG)kDefaultWidth, (LONG)kDefaultHeight };
    AdjustWindowRectEx(&rc, style, FALSE, WS_EX_TOPMOST);

    HWND hwnd = CreateWindowExW(WS_EX_TOPMOST, wc.lpszClassName, kWindowTitle, style,
                                CW_USEDEFAULT, CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top,
                                nullptr, nullptr, hInst, nullptr);
    if (!hwnd) {
        ShutdownGdiplus();
        ReleaseMutex(hInstanceMutex);
        return 1;
    }

    NONCLIENTMETRICSW ncm = { sizeof(ncm) };
    if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0)) {
        g.font = CreateFontIndirectW(&ncm.lfMessageFont);
    }
    if (!g.font) {
        g.font = CreateFontW(-24, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                             DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    }

    SetTitle(hwnd, false, 0, 0);
    PlaceTopRight(hwnd);
    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    ShutdownGdiplus();
    ReleaseMutex(hInstanceMutex);
    return (int)msg.wParam;
}
