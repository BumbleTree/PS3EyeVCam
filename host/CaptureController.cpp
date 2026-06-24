#include "CaptureController.h"

#include <mfapi.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstdarg>
#include <memory>

#include "ICameraDevice.h"
#include "DeviceRegistry.h"
#include "mfvirtualcamera_min.h"
#include "../common/FrameBus.h"
#include "../common/ControlBus.h"
#include "../common/VCamGuids.h"

using Microsoft::WRL::ComPtr;

void HostLog(const wchar_t* fmt, ...)
{
    wchar_t buf[512];
    va_list args;
    va_start(args, fmt);
    _vsnwprintf_s(buf, _TRUNCATE, fmt, args);
    va_end(args);
    if (GetConsoleWindow())
        wprintf(L"%s\n", buf);
    wchar_t line[560];
    _snwprintf_s(line, _TRUNCATE, L"PSCam4WinTray: %s\n", buf);
    OutputDebugStringW(line);
}

namespace
{

// MFCreateVirtualCamera lives in mfsensorgroup.dll (Windows 11 22000+).
// Resolved once for the whole process; the module is never freed (its
// lifetime is the process's). Thread-safe via C++11 magic-static init.
PFN_MFCreateVirtualCamera GetMFCreateVirtualCamera()
{
    static PFN_MFCreateVirtualCamera fn = []() -> PFN_MFCreateVirtualCamera {
        HMODULE module = LoadLibraryW(L"mfsensorgroup.dll");
        return module ? reinterpret_cast<PFN_MFCreateVirtualCamera>(
                            GetProcAddress(module, "MFCreateVirtualCamera"))
                      : nullptr;
    }();
    return fn;
}

} // namespace

// ---------------------------------------------------------------------------

bool CaptureController::Start(int cameraIndex, HWND notifyWnd, UINT notifyMsg)
{
    _cameraIndex = cameraIndex;
    _notifyWnd = notifyWnd;
    _notifyMsg = notifyMsg;
    {
        AcquireSRWLockExclusive(&_settingsLock);
        _desired = settings::Load(_cameraIndex);
        // Device-aware initial mode: if a recognised camera occupies this slot
        // and the persisted mode isn't one it can serve, fall back to that
        // device's default mode (e.g. an EyeToy slot persisted with the PS3's
        // 640x480@60 default). No-op for the PS3 Eye (its default mode is one of
        // its own modes) and for empty slots (no profile), so the PS3 path and
        // slot↔mode mapping are unchanged.
        if (const DeviceProfile* prof = deviceregistry::ProfileForSlot(_cameraIndex))
        {
            if (!ProfileHasMode(*prof, _desired.width, _desired.height, _desired.fps))
            {
                _desired.width  = prof->defaultMode.width;
                _desired.height = prof->defaultMode.height;
                _desired.fps    = prof->defaultMode.fps;
            }
        }
        _active = _desired;
        ReleaseSRWLockExclusive(&_settingsLock);
    }
    _stopEvent   = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    _cmdEvent    = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    _rescanEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!_stopEvent || !_cmdEvent || !_rescanEvent)
        return false;
    _thread = CreateThread(nullptr, 0, ThreadProc, this, 0, nullptr);
    return _thread != nullptr;
}

void CaptureController::Stop()
{
    if (_stopEvent)
        SetEvent(_stopEvent);
    if (_thread)
    {
        if (WaitForSingleObject(_thread, 15000) != WAIT_OBJECT_0)
            HostLog(L"camera %d: thread did not exit within 15s -- abandoning it", _cameraIndex);
        CloseHandle(_thread);
        _thread = nullptr;
    }
    if (_stopEvent)   { CloseHandle(_stopEvent);   _stopEvent = nullptr; }
    if (_cmdEvent)    { CloseHandle(_cmdEvent);    _cmdEvent = nullptr; }
    if (_rescanEvent) { CloseHandle(_rescanEvent); _rescanEvent = nullptr; }
}

void CaptureController::UpdateSettings(const Settings& s)
{
    AcquireSRWLockExclusive(&_settingsLock);
    _desired = s;
    ReleaseSRWLockExclusive(&_settingsLock);
    _settingsDirty.store(true, std::memory_order_release);
    if (_cmdEvent)
        SetEvent(_cmdEvent);
}

void CaptureController::NotifyDeviceChange()
{
    if (_rescanEvent)
        SetEvent(_rescanEvent);
}

Settings CaptureController::ActiveSettings() const
{
    AcquireSRWLockShared(&_settingsLock);
    Settings s = _active;
    ReleaseSRWLockShared(&_settingsLock);
    return s;
}

void CaptureController::SetPreviewHold(bool held)
{
    _previewHold.store(held, std::memory_order_relaxed);
    // Turning hold on must wake an Asleep controller so it re-evaluates
    // clientFresh() immediately; otherwise it stays parked in
    // WaitForMultipleObjects until some external event arrives. Turning hold
    // off needs no nudge: the next stale-keepalive check puts the camera back
    // to sleep on its own.
    if (held && _cmdEvent)
        SetEvent(_cmdEvent);
}

void CaptureController::SetState(State s)
{
    if (_state.exchange(s, std::memory_order_relaxed) != s && _notifyWnd)
        PostMessage(_notifyWnd, _notifyMsg, static_cast<WPARAM>(static_cast<int>(s)), static_cast<LPARAM>(_cameraIndex));
}

DWORD WINAPI CaptureController::ThreadProc(LPVOID self)
{
    static_cast<CaptureController*>(self)->Run();
    return 0;
}

void CaptureController::Run()
{
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION);

    Settings active = ActiveSettings();

    framebus::Writer bus;
    if (!bus.Create(_cameraIndex, active.width, active.height, active.fps, 1))
    {
        HostLog(L"FATAL: FrameBus creation failed (win32=%lu) -- not elevated?", bus.LastError());
        SetState(State::Fatal);
        MFShutdown();
        CoUninitialize();
        return;
    }
    bus.PublishBlack();

    // Publish the slot's device capabilities (modes / formats / default) before
    // any virtual-camera registration, so the DLL always has a capability block
    // to build its media types from (TDD §8.3).
    deviceregistry::PublishColdBlock(bus, _cameraIndex);

    controlbus::Host control;
    if (!control.Create(_cameraIndex))
    {
        HostLog(L"FATAL: ControlBus creation failed (win32=%lu)", GetLastError());
        SetState(State::Fatal);
        MFShutdown();
        CoUninitialize();
        return;
    }

    // ---- virtual camera registration (dynamic, slot-occupancy driven) -----
    PFN_MFCreateVirtualCamera createVCam = GetMFCreateVirtualCamera();

    ComPtr<IMFVirtualCamera> vcam;
    bool vcamLive = false;
    ULONGLONG vcamRetryDue = 0;  // GetTickCount64 deadline for next attempt
    int vcamAttempts = 0;

    auto tryRegisterVCam = [&]() {
        if (vcamLive || !createVCam || GetTickCount64() < vcamRetryDue)
            return;
        ++vcamAttempts;
        // The virtual camera advertises the name of the device that currently
        // occupies the slot ("PS3 Eye" / "PS2 EyeToy"), numbered only when more
        // than one camera of that type is present, so a lone EyeToy is just
        // "PS2 EyeToy" rather than "PS2 EyeToy #7".
        wchar_t friendlyName[64];
        deviceregistry::SlotDisplayName(_cameraIndex, friendlyName, 64);
        HRESULT hr = createVCam(MFVirtualCameraType_SoftwareCameraSource,
                                MFVirtualCameraLifetime_Session,
                                MFVirtualCameraAccess_CurrentUser,
                                friendlyName, kVCamClsidStrings[_cameraIndex], nullptr, 0, &vcam);
        if (SUCCEEDED(hr))
            hr = vcam->Start(nullptr);
        if (SUCCEEDED(hr))
        {
            vcamLive = true;
            vcamAttempts = 0;
            HostLog(L"camera %d: virtual camera registered and started", _cameraIndex);
        }
        else
        {
            vcam.Reset();
            // Right after logon MF may not be ready yet: 5 quick attempts at
            // 2s, then every 30s forever.
            vcamRetryDue = GetTickCount64() + (vcamAttempts < 5 ? 2000 : 30000);
            HostLog(L"camera %d: virtual camera registration failed 0x%08X (attempt %d)",
                    _cameraIndex, hr, vcamAttempts);
            SetState(State::VCamFailed);
        }
    };

    auto unregisterVCam = [&]() {
        if (!vcamLive && !vcam)
            return;
        if (vcam)
        {
            vcam->Stop();
            vcam->Remove();
            vcam->Shutdown();
            vcam.Reset();
        }
        vcamLive = false;
        vcamAttempts = 0;
        vcamRetryDue = 0;
        HostLog(L"camera %d: virtual camera unregistered", _cameraIndex);
    };

    if (!createVCam)
    {
        HostLog(L"FATAL: MFCreateVirtualCamera unavailable -- Windows 11 22000+ required");
        SetState(State::Fatal);
    }

    // ---- camera state ------------------------------------------------------
    // The active device for this slot (PS3 Eye today). Built by the registry on
    // wake, destroyed on sleep. AcquireFrame returns device-owned YUY2; the
    // fused Bayer->YUY2 debayer lives inside the device, no RGB intermediate.
    std::unique_ptr<ICameraDevice> device;

    // True while either (a) an external app is consuming frames (ControlBus
    // keepalive fresh) or (b) the Settings dialog preview is open for this
    // camera. Both keep the Streaming phase alive; neither touches the wire
    // protocol — previewHold is an in-process tray signal only.
    auto clientFresh = [&]() {
        const bool ext = control.ActivityAgeMs() < active.idleTimeoutMs;
        _externalClient.store(ext, std::memory_order_relaxed);
        return ext || _previewHold.load(std::memory_order_relaxed);
    };

    // Re-sync the slot's advertised capabilities (and the active mode) with the
    // device that currently occupies it. Cold path: only ever called while
    // Asleep (the camera is released), so rewriting the bus format and active
    // mode here cannot disturb a live stream. Covers a camera hot-plugged into a
    // slot that was empty — or held a different device — when Run() first
    // published the ColdBlock, including the logon race where the USB stack had
    // not finished enumerating the camera by the time Run() started.
    auto readvertiseSlot = [&](const DeviceProfile* prof) {
        // If the active/persisted mode isn't one this device can serve (e.g. an
        // EyeToy landing in a slot that defaulted to the PS3's 640x480@60), fall
        // back to the device's default so Init() gets a valid mode and the bus
        // header advertises the real geometry.
        if (!ProfileHasMode(*prof, active.width, active.height, active.fps))
        {
            active.width  = prof->defaultMode.width;
            active.height = prof->defaultMode.height;
            active.fps    = prof->defaultMode.fps;
            bus.UpdateFormat(active.width, active.height, active.fps, 1);
            _pendingMode.store(false, std::memory_order_relaxed);
            AcquireSRWLockExclusive(&_settingsLock);
            _active = active;
            ReleaseSRWLockExclusive(&_settingsLock);
            HostLog(L"camera %d: re-advertised as %s -> %ux%u@%u", _cameraIndex,
                    prof->displayName, active.width, active.height, active.fps);
        }
        // Republish the capability block from the now-known profile so the DLL
        // builds its media types from the live device (modes/formats/default
        // subtype), not whatever occupied the slot when Run() started.
        deviceregistry::PublishColdBlock(bus, _cameraIndex);
    };

    auto releaseCamera = [&]() {
        if (device)
        {
            device->Stop();   // sensor off, LED off, transfers cancelled, USB released
            device.reset();
        }
        // Refresh the slot map: drops devices that were unplugged and repairs
        // slots whose camera was replugged (stale libusb_device swapped for the
        // live one). The slot keeps the same index throughout.
        deviceregistry::Rescan();
        _fpsX10.store(0, std::memory_order_relaxed);
    };

    // Drains a UI settings snapshot. cameraLive: sensor settings are pushed to
    // the hardware immediately. Mode changes: in-place bus update while the
    // camera is off; deferred to the next sleep transition while streaming.
    auto drainSettings = [&](bool cameraLive) {
        if (!_settingsDirty.exchange(false, std::memory_order_acquire))
            return;
        Settings desired;
        {
            AcquireSRWLockShared(&_settingsLock);
            desired = _desired;
            ReleaseSRWLockShared(&_settingsLock);
        }

        if (!desired.SameMode(active))
        {
            if (!cameraLive)
            {
                bus.UpdateFormat(desired.width, desired.height, desired.fps, 1);
                active.width = desired.width;
                active.height = desired.height;
                active.fps = desired.fps;
                _pendingMode.store(false, std::memory_order_relaxed);
                HostLog(L"mode changed to %ux%u@%u", active.width, active.height, active.fps);
            }
            else
            {
                _pendingMode.store(true, std::memory_order_relaxed);
                HostLog(L"mode change queued until camera is idle");
            }
        }

        const bool sensorChanged =
            desired.flipH != active.flipH || desired.flipV != active.flipV ||
            desired.autoGain != active.autoGain || desired.gain != active.gain ||
            desired.exposure != active.exposure ||
            desired.autoWhiteBalance != active.autoWhiteBalance ||
            desired.redBalance != active.redBalance || desired.blueBalance != active.blueBalance ||
            desired.greenBalance != active.greenBalance || desired.testPattern != active.testPattern ||
            desired.brightness != active.brightness || desired.saturation != active.saturation;

        active.flipH = desired.flipH;       active.flipV = desired.flipV;
        active.autoGain = desired.autoGain; active.gain = desired.gain;
        active.exposure = desired.exposure; active.autoWhiteBalance = desired.autoWhiteBalance;
        active.idleTimeoutMs = desired.idleTimeoutMs;
        active.redBalance = desired.redBalance; active.blueBalance = desired.blueBalance;
        active.greenBalance = desired.greenBalance; active.testPattern = desired.testPattern;
        active.brightness = desired.brightness; active.saturation = desired.saturation;

        if (cameraLive && sensorChanged && device)
            device->ApplySettings(active);

        AcquireSRWLockExclusive(&_settingsLock);
        _active = active;
        ReleaseSRWLockExclusive(&_settingsLock);
    };

    auto applyPendingMode = [&]() {  // call only with the camera off
        if (!_pendingMode.load(std::memory_order_relaxed))
            return;
        _settingsDirty.store(true, std::memory_order_release);
        drainSettings(false);
    };

    // SlotEmpty (device physically absent) is distinguished from InitFailed
    // (device present but USB setup failed): the former unregisters the
    // virtual camera, the latter keeps it and retries.
    enum class WakeResult { Ok, SlotEmpty, InitFailed };
    auto wakeCamera = [&]() -> WakeResult {
        device = deviceregistry::Acquire(_cameraIndex);
        if (!device)
            return WakeResult::SlotEmpty;
        const VideoMode mode{ active.width, active.height, active.fps };
        if (!device->Init(mode))   // releases the USB handle on failure
        {
            device.reset();
            return WakeResult::InitFailed;
        }
        device->ApplySettings(active);
        if (!device->Start())
        {
            device.reset();
            return WakeResult::InitFailed;
        }
        return WakeResult::Ok;
    };

    // ---- state machine ------------------------------------------------------
    enum class Phase { Asleep, Waking, Streaming };
    Phase phase = Phase::Asleep;

    // Device profile last advertised into the ColdBlock for this slot. nullptr
    // forces the first occupied Asleep pass to (re)advertise, covering a device
    // that finished USB enumeration after Run() published the start-time block.
    const DeviceProfile* publishedProfile = nullptr;

    int consecutiveTimeouts = 0;
    uint32_t framesInWindow = 0;
    ULONGLONG windowStart = GetTickCount64();

    HANDLE asleepWaits[4] = { _stopEvent, _cmdEvent, control.WakeEvent(), _rescanEvent };
    HANDLE briefWaits[2]  = { _stopEvent, _cmdEvent };

    bool stopping = false;
    while (!stopping)
    {
        switch (phase)
        {
        case Phase::Asleep:
        {
            // Lost-wakeup-free order: reset, THEN re-check freshness.
            control.ResetWake();
            drainSettings(false);
            applyPendingMode();

            // Reconcile the virtual camera with slot occupancy: registered
            // while a physical camera is attached, gone otherwise.
            bool present = false;
            if (createVCam)
            {
                // ProfileForSlot is non-null iff the slot is occupied AND tells us
                // which device occupies it, in a single enumeration. Re-advertise
                // whenever the occupant changes (including empty -> present) so the
                // DLL always builds media types from the live device's profile.
                const DeviceProfile* prof = deviceregistry::ProfileForSlot(_cameraIndex);
                present = (prof != nullptr);
                if (present && prof != publishedProfile)
                {
                    publishedProfile = prof;
                    readvertiseSlot(prof);
                }
                if (present && !vcamLive)
                    tryRegisterVCam();
                else if (!present && vcamLive)
                    unregisterVCam();
            }

            if (vcamLive && clientFresh())
            {
                phase = Phase::Waking;
                break;
            }
            if (createVCam)
                SetState(!present ? State::CameraMissing
                                  : (vcamLive ? State::Asleep : State::VCamFailed));

            DWORD timeout = INFINITE;
            if (createVCam)
            {
                const ULONGLONG now = GetTickCount64();
                if (!present)
                    timeout = 5000;  // fallback arrival poll (device notification may race libusb)
                else if (!vcamLive)
                    timeout = vcamRetryDue > now ? static_cast<DWORD>(vcamRetryDue - now) : 0;
            }

            const DWORD r = WaitForMultipleObjects(4, asleepWaits, FALSE, timeout);
            if (r == WAIT_OBJECT_0)            // stop
                stopping = true;
            // Settings command, wake ping, device change, or timeout: loop —
            // the top of the Asleep pass re-evaluates everything.
            break;
        }

        case Phase::Waking:
        {
            drainSettings(false);
            applyPendingMode();
            if (!clientFresh())
            {
                phase = Phase::Asleep;  // client went away while we were down
                break;
            }
            SetState(State::Waking);
            const WakeResult wake = wakeCamera();
            if (wake == WakeResult::Ok)
            {
                HostLog(L"camera %d awake: %ux%u@%u", _cameraIndex, active.width, active.height, active.fps);
                SetState(State::Streaming);
                consecutiveTimeouts = 0;
                framesInWindow = 0;
                windowStart = GetTickCount64();
                phase = Phase::Streaming;
                break;
            }
            if (wake == WakeResult::SlotEmpty)
            {
                // Device physically gone: take the virtual camera offline so
                // apps stop seeing a dead "PS3 Eye" entry.
                unregisterVCam();
                SetState(State::CameraMissing);
                phase = Phase::Asleep;
                break;
            }
            SetState(State::CameraMissing);
            // Device present but USB setup failed: retry every 2s while a
            // client keeps asking; drop to sleep otherwise.
            const DWORD r = WaitForMultipleObjects(2, briefWaits, FALSE, 2000);
            if (r == WAIT_OBJECT_0)
                stopping = true;
            break;
        }

        case Phase::Streaming:
        {
            if (WaitForSingleObject(_stopEvent, 0) == WAIT_OBJECT_0)
            {
                stopping = true;
                break;
            }
            // The DLL publishes which format the active client negotiated; only
            // an MJPEG client needs the JFIF sidecar. mask==0 (no/stale client)
            // -> wantJpeg=false (TDD §5.5). One plain aligned load + branch; the
            // PS3 path always reports no jpeg and falls through to Publish().
            const bool wantJpeg =
                (control.ConsumerMask() & controlbus::kConsumeMJPEG) != 0;

            AcquiredFrame frame{};
            if (device->AcquireFrame(frame, 500, wantJpeg))
            {
                consecutiveTimeouts = 0;
                if (frame.jpeg && frame.jpegBytes)
                    bus.PublishWithJpeg(frame.yuy2, frame.yuy2Bytes, frame.jpeg, frame.jpegBytes);
                else
                    bus.Publish(frame.yuy2, frame.yuy2Bytes);

                ++framesInWindow;
                const ULONGLONG now = GetTickCount64();
                if (now - windowStart >= 2000)
                {
                    _fpsX10.store(static_cast<uint32_t>(framesInWindow * 10000ull / (now - windowStart)),
                                  std::memory_order_relaxed);
                    framesInWindow = 0;
                    windowStart = now;
                }
            }
            else if (++consecutiveTimeouts >= 4)
            {
                HostLog(L"camera %d stopped delivering frames -- device lost?", _cameraIndex);
                releaseCamera();
                phase = Phase::Waking;  // immediate retry covers replug
                break;
            }

            drainSettings(true);

            // Go to sleep when there are no clients, OR when the preview is
            // the only consumer and a mode change is pending (applying it
            // requires releasing the camera; preview-hold re-wakes it
            // immediately afterwards with the new format).
            const bool previewOnlyModeChange =
                _pendingMode.load(std::memory_order_relaxed) &&
                IsPreviewOnly();

            if (previewOnlyModeChange || !clientFresh())
            {
                if (previewOnlyModeChange)
                    HostLog(L"camera %d: applying pending mode change for preview",
                            _cameraIndex);
                else
                    HostLog(L"camera %d: no clients for %ums and preview closed -- going to sleep",
                            _cameraIndex, active.idleTimeoutMs);
                releaseCamera();
                bus.PublishBlack();
                applyPendingMode();
                phase = Phase::Asleep;
            }
            break;
        }
        }
    }

    // ---- ordered teardown ---------------------------------------------------
    releaseCamera();
    bus.PublishBlack();
    unregisterVCam();
    control.Close();
    bus.Close();
    MFShutdown();
    CoUninitialize();
    HostLog(L"camera %d thread exited", _cameraIndex);
}
