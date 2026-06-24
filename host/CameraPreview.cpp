#include "CameraPreview.h"

#include <commctrl.h>
#include <cstring>

#include "CaptureController.h"
#include "FrameBusPreviewSource.h"
#include "../common/FrameBus.h"
#include "../common/Settings.h"
#include "../res/resource.h"

namespace
{
// Custom message posted by the worker to the preview window when the source
// format changes, so the UI thread can resize the control to match the aspect
// ratio (eliminates black bars without distorting the image).
constexpr UINT WM_PREVIEW_RESIZE = WM_USER + 0x100;

// YUY2 (BT.601 limited range) -> BGRA, 2 pixels per 8-byte YUY2 macro-pair.
// Output is full-range. No SIMD on purpose: at 640x480@60 this is ~18 MB/s
// and only runs while the Settings dialog is open.
inline void Yuy2ToBgra(const uint8_t* src, uint8_t* dst, int pixels)
{
    auto clamp = [](int c) { return c < 0 ? 0 : (c > 255 ? 255 : c); };
    for (int i = 0; i < pixels; i += 2)
    {
        // Limited-range Y' expansion: Y' in [16..235] -> [0..255]
        const int y0 = (src[0] - 16) * 255 / 219;
        const int u  = src[1] - 128;
        const int y1 = (src[2] - 16) * 255 / 219;
        const int v  = src[3] - 128;
        src += 4;

        dst[0] = static_cast<uint8_t>(clamp(y0 + (453 * u) / 256));
        dst[1] = static_cast<uint8_t>(clamp(y0 - ( 88 * u) / 256 - (183 * v) / 256));
        dst[2] = static_cast<uint8_t>(clamp(y0 + (359 * v) / 256));
        dst[3] = 255;
        dst[4] = static_cast<uint8_t>(clamp(y1 + (453 * u) / 256));
        dst[5] = static_cast<uint8_t>(clamp(y1 - ( 88 * u) / 256 - (183 * v) / 256));
        dst[6] = static_cast<uint8_t>(clamp(y1 + (359 * v) / 256));
        dst[7] = 255;
        dst += 8;
    }
}

// Shared font helper for the badge and the "waiting" fallback text.
HFONT DialogFont(HWND wnd)
{
    HFONT fnt = reinterpret_cast<HFONT>(SendMessageW(wnd, WM_GETFONT, 0, 0));
    return fnt ? fnt : static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
}
} // namespace

CameraPreview::CameraPreview() = default;

CameraPreview::~CameraPreview()
{
    Detach();
}

void CameraPreview::SetControllerArray(CaptureController* controllers)
{
    _controllers = controllers;
}

void CameraPreview::Attach(HWND parent, int ctrlId, HINSTANCE /*inst*/)
{
    _parent = parent;
    _wnd    = GetDlgItem(parent, ctrlId);
    if (!_wnd)
        return;

    // Capture the dialog-template cell before any subclassing/resize so
    // ResizeToAspect always anchors here (no drift across format changes).
    GetWindowRect(_wnd, &_cell);
    MapWindowPoints(nullptr, parent, reinterpret_cast<LPPOINT>(&_cell), 2);

    SetWindowSubclass(_wnd, &CameraPreview::SubclassProc, 1,
                      reinterpret_cast<DWORD_PTR>(this));

    // Fit to 4:3 immediately so the dialog opens with correct proportions
    // even before the first frame arrives.
    ResizeToAspect();

    _stopEvent = CreateEventW(nullptr, TRUE /*manual-reset*/, FALSE, nullptr);
    if (!_stopEvent)
        return;

    _running.store(true, std::memory_order_relaxed);
    _thread = CreateThread(nullptr, 0, &CameraPreview::WorkerProc, this, 0, nullptr);
}

void CameraPreview::Detach()
{
    const int cam = _cameraIndex.load(std::memory_order_relaxed);
    if (_controllers && cam >= 0)
        (_controllers + cam)->SetPreviewHold(false);

    StopWorker();

    if (_wnd)
    {
        RemoveWindowSubclass(_wnd, &CameraPreview::SubclassProc, 1);
        _wnd = nullptr;
    }

    // Tear down DIB on the UI thread (worker is now joined).
    AcquireSRWLockExclusive(&_dibLock);
    if (_dibDc) { SelectObject(_dibDc, _dibOld); DeleteDC(_dibDc); _dibDc = nullptr; _dibOld = nullptr; }
    if (_dib)   { DeleteObject(_dib); _dib = nullptr; }
    _dibBits = nullptr;
    _dibW = _dibH = 0;
    _inUse.store(false, std::memory_order_relaxed);
    ReleaseSRWLockExclusive(&_dibLock);

    // Back buffer is UI-thread-only; the worker is joined, so no lock needed.
    DestroyBackBuffer();
}

void CameraPreview::StopWorker()
{
    _running.store(false, std::memory_order_relaxed);
    if (_stopEvent)
        SetEvent(_stopEvent);
    if (_thread)
    {
        WaitForSingleObject(_thread, 2000);
        CloseHandle(_thread);
        _thread = nullptr;
    }
    if (_stopEvent)
    {
        CloseHandle(_stopEvent);
        _stopEvent = nullptr;
    }
}

void CameraPreview::SetCamera(int cameraIndex)
{
    const int oldIdx = _cameraIndex.exchange(cameraIndex, std::memory_order_relaxed);
    if (_controllers && oldIdx >= 0 && oldIdx != cameraIndex)
        (_controllers + oldIdx)->SetPreviewHold(false);
    if (_controllers && cameraIndex >= 0)
        (_controllers + cameraIndex)->SetPreviewHold(true);
    // Clear stale badge state from the previous camera; the worker refreshes
    // it once the new source delivers its first frame.
    _inUse.store(false, std::memory_order_relaxed);
}

bool CameraPreview::RecreateDib(int width, int height)
{
    if (width <= 0 || height <= 0)
        return false;
    if (width == _dibW && height == _dibH && _dib)
        return true;

    AcquireSRWLockExclusive(&_dibLock);

    if (_dibDc) { SelectObject(_dibDc, _dibOld); DeleteDC(_dibDc); _dibDc = nullptr; _dibOld = nullptr; }
    if (_dib)   { DeleteObject(_dib); _dib = nullptr; }
    _dibBits = nullptr;

    BITMAPINFO bi{};
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = width;
    bi.bmiHeader.biHeight      = -height;   // top-down
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    _dib = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &_dibBits, nullptr, 0);
    if (_dib)
    {
        _dibDc  = CreateCompatibleDC(nullptr);
        _dibOld = static_cast<HBITMAP>(SelectObject(_dibDc, _dib));
        if (_dibBits)
            std::memset(_dibBits, 0, static_cast<size_t>(width) * height * 4);
        // Only stamp the new dims on a fully-built buffer so a transient
        // CreateDIBSection failure is retried on the next worker pass.
        _dibW = width;
        _dibH = height;
    }
    else
    {
        _dibW = _dibH = 0;
    }

    ReleaseSRWLockExclusive(&_dibLock);
    return _dib != nullptr;
}

LRESULT CALLBACK CameraPreview::SubclassProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                                             UINT_PTR idSubclass, DWORD_PTR refData)
{
    auto* self = reinterpret_cast<CameraPreview*>(refData);
    switch (msg)
    {
    case WM_PREVIEW_RESIZE:
        if (self) self->ResizeToAspect();
        return 0;
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        if (self)
        {
            RECT rc;
            GetClientRect(hwnd, &rc);
            self->Paint(hdc, rc.right - rc.left, rc.bottom - rc.top);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;  // we paint everything
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

void CameraPreview::ResizeToAspect()
{
    if (!_wnd || !_parent)
        return;

    // Source aspect from the current DIB. Falls back to 4:3 (the only modes
    // the PS3 Eye sensor produces) before the first frame arrives. Read under
    // the shared lock: RecreateDib (worker thread) mutates these under the
    // exclusive lock, and the WM_PREVIEW_RESIZE that brought us here is
    // posted after RecreateDib returns, so the values are stable by now.
    int srcW, srcH;
    {
        AcquireSRWLockShared(&_dibLock);
        srcW = _dibW;
        srcH = _dibH;
        ReleaseSRWLockShared(&_dibLock);
    }
    if (srcW <= 0 || srcH <= 0) { srcW = 4; srcH = 3; }

    const int cellX = _cell.left;
    const int cellY = _cell.top;
    const int cellW = _cell.right  - _cell.left;
    const int cellH = _cell.bottom - _cell.top;
    if (cellW <= 0 || cellH <= 0)
        return;

    // Fill the full cell width and derive the height from the true source
    // aspect, so the preview is a gap-free srcW:srcH rectangle — no letterbox
    // bars, no distortion. The dialog cell is sized ~4:3 (see app.rc) so the
    // height lands just inside it; any spare pixels sit below the image as plain
    // dialog background, never as bars framing it. If the derived height would
    // overflow a very short cell, clamp to the height and narrow the width.
    int w = cellW;
    int h = (cellW * srcH) / srcW;
    if (h > cellH)
    {
        h = cellH;
        w = (cellH * srcW) / srcH;
    }
    const int x = cellX + (cellW - w) / 2;   // centred horizontally (== cellX when w == cellW)
    const int y = cellY;                      // anchored at the top

    SetWindowPos(_wnd, nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
    InvalidateRect(_wnd, nullptr, FALSE);
}

bool CameraPreview::EnsureBackBuffer(HDC ref, int w, int h)
{
    if (_backDc && _backW == w && _backH == h)
        return true;
    DestroyBackBuffer();
    if (w <= 0 || h <= 0)
        return false;

    _backDc = CreateCompatibleDC(ref);
    if (!_backDc)
        return false;
    // NOTE: the bitmap must be compatible with the WINDOW DC (ref), not the
    // memory DC — a bitmap "compatible" with a fresh memory DC is 1bpp
    // monochrome, which would turn the preview black-and-white.
    _backBmp = CreateCompatibleBitmap(ref, w, h);
    if (!_backBmp)
    {
        DeleteDC(_backDc);
        _backDc = nullptr;
        return false;
    }
    _backOld = static_cast<HBITMAP>(SelectObject(_backDc, _backBmp));
    _backW = w;
    _backH = h;
    return true;
}

void CameraPreview::DestroyBackBuffer()
{
    if (_backDc)  { SelectObject(_backDc, _backOld); DeleteDC(_backDc); _backDc = nullptr; _backOld = nullptr; }
    if (_backBmp) { DeleteObject(_backBmp); _backBmp = nullptr; }
    _backW = _backH = 0;
}

void CameraPreview::Paint(HDC hdc, int w, int h)
{
    if (w <= 0 || h <= 0)
        return;

    // Compose the whole frame off-screen, then blit it to the window in one
    // shot. Drawing the scaled frame and the "in use" badge straight to the
    // window DC ~60x/sec made the badge (overdrawn by every new frame and then
    // redrawn) flicker, and pushed a full StretchBlt to the screen each frame.
    // A single BitBlt of a fully-composed buffer updates the screen atomically:
    // no flicker, and the only screen-bound work per frame is one BitBlt.
    const HDC dc = EnsureBackBuffer(hdc, w, h) ? _backDc : hdc;

    // Hold the shared lock for the scale. The worker takes it exclusive only
    // inside RecreateDib (rare) and for the ~1ms write+convert, so contention
    // is negligible; this guarantees the source DIB is neither destroyed nor
    // mutated mid-read.
    AcquireSRWLockShared(&_dibLock);
    const bool haveFrame = _dibDc && _dibW > 0 && _dibH > 0;
    if (haveFrame)
    {
        // High-quality interpolated scale. The preview box is smaller than the
        // 640x480 source, so this is a shrink; GDI's default BLACKONWHITE mode
        // boolean-ANDs the eliminated pixels when shrinking a colour image,
        // darkening it and shredding fine detail — which made the preview look
        // far worse than the real stream (most visible on the sharp PS3 Eye).
        // HALFTONE averages the source pixels for a faithful reproduction; it
        // requires the brush origin to be reset to avoid mis-aligned dithering.
        SetStretchBltMode(dc, HALFTONE);
        SetBrushOrgEx(dc, 0, 0, nullptr);
        StretchBlt(dc, 0, 0, w, h, _dibDc, 0, 0, _dibW, _dibH, SRCCOPY);
    }
    const bool inUse = _inUse.load(std::memory_order_relaxed);
    ReleaseSRWLockShared(&_dibLock);

    HFONT oldFnt = static_cast<HFONT>(SelectObject(dc, DialogFont(_wnd)));

    if (!haveFrame)
    {
        RECT rc{ 0, 0, w, h };
        FillRect(dc, &rc, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
        const wchar_t* text = L"Waiting for camera...";
        SetTextColor(dc, RGB(170, 170, 170));
        SetBkMode(dc, TRANSPARENT);
        DrawTextW(dc, text, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    else if (inUse)
    {
        // "In use by another app" badge. Composited into the back buffer with
        // the frame, so it never flickers. Drawn after the blit so it sits on
        // top.
        const wchar_t* text = L"  in use by another app  ";
        SIZE sz{};
        GetTextExtentPoint32W(dc, text, static_cast<int>(wcslen(text)), &sz);
        const int bw = sz.cx + 8, bh = sz.cy + 4;
        RECT badge{ 6, 4, 6 + bw, 4 + bh };
        const HBRUSH bg = CreateSolidBrush(RGB(0, 0, 0));
        FillRect(dc, &badge, bg);
        DeleteObject(bg);
        SetTextColor(dc, RGB(255, 220, 0));
        SetBkMode(dc, TRANSPARENT);
        DrawTextW(dc, text, -1, &badge, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }

    SelectObject(dc, oldFnt);

    // Present the composed buffer in a single atomic blit. (When the back
    // buffer could not be created we drew straight to the window above, so dc
    // == hdc and there is nothing to present.)
    if (dc != hdc)
        BitBlt(hdc, 0, 0, w, h, _backDc, 0, 0, SRCCOPY);
}

DWORD WINAPI CameraPreview::WorkerProc(LPVOID selfp)
{
    auto* self = static_cast<CameraPreview*>(selfp);

    // Staging buffer for raw YUY2 from the source.
    std::unique_ptr<uint8_t[]> staging(new uint8_t[framebus::kMaxFrameBytes]);
    LONG64 lastFrameId = 0;
    uint32_t lastFmtW = 0, lastFmtH = 0;
    int openForCamera = -1;
    ULONGLONG lastInUseCheck = 0;

    while (self->_running.load(std::memory_order_relaxed))
    {
        const int cam = self->_cameraIndex.load(std::memory_order_relaxed);
        if (cam < 0)
        {
            WaitForSingleObject(self->_stopEvent, 100);
            continue;
        }

        // (Re)open the source whenever the target camera changes. This is
        // the only place _source is mutated — the UI thread touches only the
        // atomic _cameraIndex, never the source directly.
        if (cam != openForCamera)
        {
            if (self->_source)
                self->_source->Close();
            else
                self->_source = std::make_unique<FrameBusPreviewSource>();
            if (!self->_source->TryOpen(cam))
            {
                // Producer not ready (host still starting up, or dialog just
                // opened before the controller registered the section). Back
                // off briefly and retry.
                WaitForSingleObject(self->_stopEvent, 200);
                continue;
            }
            openForCamera = cam;
            lastFrameId = 0;          // new source — re-read current frame
            lastFmtW = lastFmtH = 0;
            self->_inUse.store(false, std::memory_order_relaxed);
            lastInUseCheck = 0;
        }

        // Read format; recreate DIB if it changed.
        uint32_t w = 0, h = 0, fps = 0;
        if (self->_source->ReadFormat(w, h, fps) &&
            (w != lastFmtW || h != lastFmtH))
        {
            if (self->RecreateDib(static_cast<int>(w), static_cast<int>(h)))
            {
                lastFmtW = w;
                lastFmtH = h;
                lastFrameId = 0;   // dims changed — accept the next frame unconditionally
                // Ask the UI thread to refit the control to the new aspect
                // ratio. Posted (not sent) so the worker never blocks on the
                // UI thread.
                PostMessageW(self->_wnd, WM_PREVIEW_RESIZE, 0, 0);
            }
        }

        // Block until a new frame *might* be available. Bounded so we still
        // notice _running flips and cameraIndex changes quickly.
        self->_source->WaitFrame(150);
        if (!self->_running.load(std::memory_order_relaxed))
            break;
        if (self->_cameraIndex.load(std::memory_order_relaxed) != openForCamera)
            continue;  // SetCamera happened during the wait — re-target

        // Refresh the in-use badge flag for the UI thread. The worker owns
        // _source; doing this here keeps Paint off _source entirely and
        // avoids opening the ControlBus section on every WM_PAINT.
        const ULONGLONG nowTick = GetTickCount64();
        if (self->_controllers && nowTick - lastInUseCheck >= 500)
        {
            lastInUseCheck = nowTick;
            const uint32_t idleMs = (self->_controllers + cam)
                                        ->ActiveSettings().idleTimeoutMs;
            self->_inUse.store(self->_source->IsCameraInUse(idleMs),
                               std::memory_order_relaxed);
        }

        const uint32_t dstBytes = framebus::Yuy2Bytes(w, h);
        if (dstBytes == 0 || dstBytes > framebus::kMaxFrameBytes)
            continue;

        const LONG64 id = self->_source->TryReadNewer(staging.get(), dstBytes, lastFrameId);
        if (id == 0)
            continue;

        lastFrameId = id;

        // Convert YUY2 -> BGRA into the DIB bits under the exclusive lock.
        // Paint holds the lock shared during the blit, so this guarantees
        // Paint never blits from a buffer the worker is writing. The write
        // is ~1ms at 640x480 — negligible contention.
        AcquireSRWLockExclusive(&self->_dibLock);
        const bool ok = self->_dibBits &&
                        self->_dibW == static_cast<int>(w) &&
                        self->_dibH == static_cast<int>(h);
        if (ok)
        {
            Yuy2ToBgra(staging.get(), static_cast<uint8_t*>(self->_dibBits),
                       static_cast<int>(w) * static_cast<int>(h));
        }
        ReleaseSRWLockExclusive(&self->_dibLock);
        if (ok)
            InvalidateRect(self->_wnd, nullptr, FALSE);
    }

    return 0;
}
