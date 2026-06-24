#pragma once
//
// CameraPreview — a self-contained live-preview widget for the Settings dialog.
//
// Owns a child static window (created from the IDC_PREVIEW control in the
// dialog template and subclassed so we can paint it ourselves), a dedicated
// worker thread that blocks on the camera's FrameReady auto-reset event, and a
// DIB section the worker writes converted BGRA into while WM_PAINT blits from.
//
// Threading model:
//   * Public methods (Attach/Detach/SetCamera/SetControllerArray) are called
//     on the dialog (UI) thread only.
//   * The worker thread owns _source and writes converted BGRA into the DIB
//     under an exclusive SRWLOCK (held only for the ~1ms write). WM_PAINT
//     reads the DIB under a shared SRWLOCK (held only for the StretchBlt) so
//     it never sees a half-written frame. The worker also refreshes an _inUse
//     atomic for the badge so the UI thread never touches _source.
//
// A single DIB section is sufficient: the exclusive/shared SRWLOCK pair already
// guarantees tear-free reads, so a second buffer and an atomic front-index
// swap would buy nothing but extra GDI churn.
//
// Zero-overhead guarantee: when the dialog is closed, Detach() signals the
// worker, joins it, tears down the DIB, and clears the preview-hold flag on
// the camera. No thread, no Reader mapping, no held camera survives.
//
#include <windows.h>
#include <atomic>
#include <memory>
#include <cstdint>

#include "ICameraPreviewSource.h"
class CaptureController;

class CameraPreview
{
public:
    CameraPreview();
    ~CameraPreview();

    // Take ownership of the IDC_PREVIEW control in the dialog. Subclasses it,
    // fits it to the source aspect ratio, and starts the worker thread.
    void Attach(HWND parent, int ctrlId, HINSTANCE inst);
    // Signals the worker, joins it, removes the subclass, tears down the DIB,
    // and clears the preview-hold flag. Idempotent.
    void Detach();

    // Re-target the worker at a new camera. Toggles the per-camera
    // preview-hold flags; the worker re-opens the source on its next loop.
    void SetCamera(int cameraIndex);

    // CaptureController array base (indexed by cameraIndex) — used to toggle
    // the in-process preview-hold flag that keeps the camera awake while the
    // dialog is open.
    void SetControllerArray(CaptureController* controllers);

private:
    static LRESULT CALLBACK SubclassProc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
    static DWORD WINAPI WorkerProc(LPVOID self);

    void StopWorker();                       // signal + join
    bool RecreateDib(int width, int height); // worker thread only; false on failure
    void ResizeToAspect();                   // fit the control to the source's aspect ratio
    void Paint(HDC hdc, int w, int h);
    bool EnsureBackBuffer(HDC ref, int w, int h);  // UI thread only; false on failure
    void DestroyBackBuffer();                       // UI thread only

    // Worker state
    std::unique_ptr<ICameraPreviewSource> _source;  // worker-thread only
    HANDLE _thread      = nullptr;
    HANDLE _stopEvent   = nullptr;   // manual-reset
    std::atomic<int>    _cameraIndex{ -1 };
    std::atomic<bool>   _running{ false };
    // Badge state, refreshed by the worker once per frame and read by Paint.
    // Avoids the UI thread touching _source (which the worker owns) and
    // avoids opening the ControlBus section on every WM_PAINT.
    std::atomic<bool>   _inUse{ false };

    // UI state
    HWND   _parent       = nullptr;
    HWND   _wnd          = nullptr;
    CaptureController* _controllers = nullptr;

    // Original layout cell (dialog-template units -> client pixels), captured
    // once in Attach. ResizeToAspect fits the control to the source aspect
    // ratio inside this cell, so the control never outgrows its allotted space
    // and never accumulates drift across repeated resizes.
    RECT   _cell{ 0, 0, 0, 0 };

    // DIB section: worker converts into the bits under an exclusive SRWLOCK,
    // WM_PAINT blits from them under a shared SRWLOCK. RecreateDib (worker
    // thread) takes the same lock exclusive to tear down and rebuild when the
    // format changes. _dibW/_dibH are only updated on a successful rebuild so
    // a transient CreateDIBSection failure is retried on the next pass.
    SRWLOCK     _dibLock = SRWLOCK_INIT;
    HBITMAP     _dib     = nullptr;
    void*       _dibBits = nullptr;
    HDC         _dibDc   = nullptr;
    HBITMAP     _dibOld  = nullptr;
    int         _dibW    = 0;
    int         _dibH    = 0;

    // Off-screen compose buffer for flicker-free painting. Touched only on the
    // UI thread (Paint/Detach), so it needs no lock — the worker never sees it.
    // Paint() scales the frame and stamps the badge into this buffer, then
    // BitBlts it to the window in a single shot; that makes on-screen updates
    // atomic, so neither the live image nor the "in use" badge flickers. Sized
    // to the control's client area and rebuilt on resize; torn down in Detach.
    HDC         _backDc  = nullptr;
    HBITMAP     _backBmp = nullptr;
    HBITMAP     _backOld = nullptr;
    int         _backW   = 0;
    int         _backH   = 0;
};
