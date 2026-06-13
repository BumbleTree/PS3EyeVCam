#pragma once
//
// CaptureController — the camera thread. Owns everything hardware- and
// MF-related: the PS3EYECam object, the FrameBus writer, the ControlBus host,
// and the IMFVirtualCamera registration. Runs the sleep/wake state machine:
//
//   ASLEEP    camera fully released (USB closed, LED off, 0% CPU); waiting on
//             the ControlBus wake event the DLL pulses when a client streams
//   WAKING    re-enumerate + init + start (~0.7s, clients see black frames)
//   STREAMING getFrame (fused Bayer->YUY2 debayer) -> FrameBus publish; back
//             to ASLEEP when the DLL keepalive goes stale (no client for
//             idleTimeoutMs)
//
// The IMFVirtualCamera registration is dynamic: it exists only while a
// physical PS3 Eye occupies this slot, so apps see exactly as many "PS3 Eye"
// cameras as are plugged in. Arrival/removal is signaled by the tray window's
// device-interface notification through NotifyDeviceChange(), with a slow
// fallback poll while the slot is empty.
//
// The UI thread NEVER touches the camera: it calls UpdateSettings() with a
// full snapshot; this thread drains it at safe points. Mode changes apply
// immediately while asleep and are deferred to the next sleep transition
// while streaming (changing the bus format under a live client would freeze
// that client's picture).
//
#include <windows.h>
#include <atomic>
#include "../common/Settings.h"

class CaptureController
{
public:
    enum class State : int
    {
        Starting = 0,
        Asleep,         // virtual camera registered, physical camera off
        Waking,
        Streaming,
        CameraMissing,  // PS3 Eye not found / unplugged (retrying)
        VCamFailed,     // MFCreateVirtualCamera/Start failing (retrying)
        Fatal,          // unrecoverable (e.g. shared memory creation failed)
    };

    // Begins the camera thread. State changes are announced with
    // PostMessage(notifyWnd, notifyMsg, (WPARAM)State, (LPARAM)cameraIndex).
    bool Start(int cameraIndex, HWND notifyWnd, UINT notifyMsg);
    void Stop();  // signals and joins the thread; safe to call twice

    // Full-snapshot settings handoff from the UI thread.
    void UpdateSettings(const Settings& s);

    // Pokes the camera thread to re-evaluate slot occupancy (called by the
    // tray window on WM_DEVICECHANGE for the PS3 Eye interface class).
    void NotifyDeviceChange();

    State    GetState() const { return _state.load(std::memory_order_relaxed); }
    bool     HasPendingModeChange() const { return _pendingMode.load(std::memory_order_relaxed); }
    // Capture rate over the last measurement window, x10 (594 == 59.4 fps).
    uint32_t MeasuredFpsX10() const { return _fpsX10.load(std::memory_order_relaxed); }
    Settings ActiveSettings() const;

private:
    static DWORD WINAPI ThreadProc(LPVOID self);
    void Run();
    void SetState(State s);

    int    _cameraIndex = 0;
    HWND   _notifyWnd = nullptr;
    UINT   _notifyMsg = 0;
    HANDLE _thread = nullptr;
    HANDLE _stopEvent = nullptr;   // manual-reset
    HANDLE _cmdEvent = nullptr;    // auto-reset, pulsed by UpdateSettings
    HANDLE _rescanEvent = nullptr; // auto-reset, pulsed by NotifyDeviceChange

    mutable SRWLOCK _settingsLock = SRWLOCK_INIT;
    Settings _desired;             // written by UI thread
    Settings _active;              // owned by camera thread, mirrored for UI reads

    std::atomic<State>    _state{ State::Starting };
    std::atomic<bool>     _settingsDirty{ false };
    std::atomic<bool>     _pendingMode{ false };
    std::atomic<uint32_t> _fpsX10{ 0 };
};

// printf when a console is attached (--console) + OutputDebugString always.
void HostLog(const wchar_t* fmt, ...);
