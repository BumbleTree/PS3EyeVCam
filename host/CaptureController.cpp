#include "CaptureController.h"

#include <mfapi.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstdarg>
#include <memory>

#include "ps3eye.h"
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
    _snwprintf_s(line, _TRUNCATE, L"PS3EyeVCamTray: %s\n", buf);
    OutputDebugStringW(line);
}

namespace
{

static SRWLOCK g_devicesLock = SRWLOCK_INIT;

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

// Push every sensor-level setting to the camera (order-safe live or pre-start).
void ApplySensorSettings(const ps3eye::PS3EYECam::PS3EYERef& eye, const Settings& s)
{
    eye->setFlip(s.flipH, s.flipV);
    eye->setAutoWhiteBalance(s.autoWhiteBalance);
    if (s.autoGain)
    {
        eye->setAutogain(true);
    }
    else
    {
        eye->setAutogain(false);  // also re-applies cached gain/exposure
        eye->setGain(static_cast<uint8_t>(s.gain));
        eye->setExposure(static_cast<uint8_t>(s.exposure));
    }

    if (!s.autoWhiteBalance)
    {
        eye->setRedBalance(static_cast<uint8_t>(s.redBalance));
        eye->setBlueBalance(static_cast<uint8_t>(s.blueBalance));
        eye->setGreenBalance(static_cast<uint8_t>(s.greenBalance));
    }

    eye->setTestPattern(s.testPattern);
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
        HRESULT hr = createVCam(MFVirtualCameraType_SoftwareCameraSource,
                                MFVirtualCameraLifetime_Session,
                                MFVirtualCameraAccess_CurrentUser,
                                kVCamFriendlyNames[_cameraIndex], kVCamClsidStrings[_cameraIndex], nullptr, 0, &vcam);
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
    ps3eye::PS3EYECam::PS3EYERef eye;

    // getFrame writes publish-ready YUY2 straight into this buffer (the
    // Bayer->YUY2 debayer is fused inside the driver; no RGB intermediate).
    std::unique_ptr<uint8_t[]> yuy2(new uint8_t[framebus::kMaxFrameBytes]);

    // True while a physical PS3 Eye occupies this slot. Refreshes the global
    // slot map (cheap libusb descriptor walk; only runs while not streaming).
    auto slotOccupied = [&]() -> bool {
        AcquireSRWLockExclusive(&g_devicesLock);
        const auto& devices = ps3eye::PS3EYECam::getDevices(true);
        const bool present = devices.size() > static_cast<size_t>(_cameraIndex) &&
                             devices[_cameraIndex] != nullptr;
        ReleaseSRWLockExclusive(&g_devicesLock);
        return present;
    };

    auto releaseCamera = [&]() {
        if (eye)
        {
            eye->stop();      // sensor off, LED off, transfers cancelled, USB interface released
            eye.reset();
        }
        // Refresh the slot map: drops devices that were unplugged and repairs
        // slots whose camera was replugged (stale libusb_device swapped for
        // the live one). The slot keeps the same index throughout.
        AcquireSRWLockExclusive(&g_devicesLock);
        ps3eye::PS3EYECam::getDevices(true);
        ReleaseSRWLockExclusive(&g_devicesLock);
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
            desired.greenBalance != active.greenBalance || desired.testPattern != active.testPattern;

        active.flipH = desired.flipH;       active.flipV = desired.flipV;
        active.autoGain = desired.autoGain; active.gain = desired.gain;
        active.exposure = desired.exposure; active.autoWhiteBalance = desired.autoWhiteBalance;
        active.idleTimeoutMs = desired.idleTimeoutMs;
        active.redBalance = desired.redBalance; active.blueBalance = desired.blueBalance;
        active.greenBalance = desired.greenBalance; active.testPattern = desired.testPattern;

        if (cameraLive && sensorChanged && eye)
            ApplySensorSettings(eye, active);

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
        AcquireSRWLockExclusive(&g_devicesLock);
        const auto& devices = ps3eye::PS3EYECam::getDevices(true);
        if (devices.size() <= static_cast<size_t>(_cameraIndex) || !devices[_cameraIndex])
        {
            ReleaseSRWLockExclusive(&g_devicesLock);
            return WakeResult::SlotEmpty;
        }
        eye = devices[_cameraIndex];
        ReleaseSRWLockExclusive(&g_devicesLock);
        if (!eye->init(active.width, active.height, static_cast<uint16_t>(active.fps),
                       ps3eye::PS3EYECam::EOutputFormat::YUY2))
        {
            eye->release();   // close the USB handle; don't hold it while idle
            eye.reset();
            return WakeResult::InitFailed;
        }
        ApplySensorSettings(eye, active);
        if (!eye->start())
        {
            eye->release();
            eye.reset();
            return WakeResult::InitFailed;
        }
        return WakeResult::Ok;
    };

    // ---- state machine ------------------------------------------------------
    enum class Phase { Asleep, Waking, Streaming };
    Phase phase = Phase::Asleep;

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
                present = slotOccupied();
                if (present && !vcamLive)
                    tryRegisterVCam();
                else if (!present && vcamLive)
                    unregisterVCam();
            }

            if (vcamLive && control.ActivityAgeMs() < active.idleTimeoutMs)
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
            if (control.ActivityAgeMs() >= active.idleTimeoutMs)
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
            if (eye->getFrame(yuy2.get(), 500))
            {
                consecutiveTimeouts = 0;
                bus.Publish(yuy2.get(), framebus::Yuy2Bytes(active.width, active.height));

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

            if (control.ActivityAgeMs() >= active.idleTimeoutMs)
            {
                HostLog(L"camera %d: no clients for %ums -- going to sleep", _cameraIndex, active.idleTimeoutMs);
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
