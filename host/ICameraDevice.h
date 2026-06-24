#pragma once
//
// ICameraDevice — the capture-side abstraction. Mirrors ICameraPreviewSource on
// the producer side: it hides USB, sensor bring-up, and (for JPEG cameras) the
// decode, behind a transport-agnostic interface. CaptureController stays the
// orchestrator (state machine, FrameBus, ControlBus, IMFVirtualCamera) and never
// switches on a product enum — it drives whatever ICameraDevice the
// DeviceRegistry hands it for a slot, parameterised by a static DeviceProfile.
//
// Phase 1 ships exactly one implementation (Ps3EyeDevice, wrapping the vendored
// PS3EYEDriver) and preserves the existing PS3 Eye behaviour byte-for-byte. The
// EyeToy (WinUSB isochronous via libusb, JPEG->YUY2) is added later behind the
// same interface — see docs/IMPLEMENTATION_PLAN.md.
//
#include <cstdint>
#include "../common/Settings.h"   // VideoMode, Settings, kVideoModes

// How a device's frames reach us. Both PS3 Eye and EyeToy ride libusb's WinUSB
// backend (bulk vs isochronous); kept distinct because their capture loops and
// per-frame work differ. UVC/USB3 are reserved for future cameras.
enum class TransportClass : uint32_t
{
    None           = 0,
    UsbBulk_Libusb = 1,   // PS3 Eye  — WinUSB bulk
    UsbIso_Libusb  = 2,   // EyeToy   — WinUSB isochronous (libusb 1.0.27)
    UVC_Inbox      = 3,   // PS4 (future) — inbox usbvideo.sys, no libusb
    Usb3_Custom    = 4,   // PS5 (future)
};

// Pixel-format bits, shared by DeviceProfile::formatMask (what a device can
// serve) and ControlBus consumerMask (what a client is consuming).
enum FormatBits : uint32_t
{
    FMT_YUY2  = 1u,
    FMT_NV12  = 2u,
    FMT_MJPEG = 4u,
};

// Which Settings-dialog sensor controls a device actually backs in its
// ApplySettings(). DeviceProfile::controlMask drives which controls the dialog
// enables; a device greys out everything it does not advertise here, so users
// only ever touch settings that take effect (TDD §8.x). Video-mode selection is
// always available and comes from DeviceProfile::modes, not this mask.
enum ControlBits : uint32_t
{
    CTRL_FLIP        = 1u << 0,   // horizontal / vertical flip
    CTRL_GAIN        = 1u << 1,   // auto gain/exposure + manual gain + exposure
    CTRL_WHITEBAL    = 1u << 2,   // auto white balance toggle (AWB on/off)
    CTRL_TESTPATTERN = 1u << 3,
    CTRL_WB_MANUAL   = 1u << 4,   // manual R/G/B balance gains (when AWB off)
    CTRL_BRIGHTNESS  = 1u << 5,   // brightness slider (EyeToy OV7648 reg 0x06)
    CTRL_SATURATION  = 1u << 6,   // saturation slider (EyeToy OV7648 reg 0x03)
};

// Static, data-only description of a supported camera. New cameras add a profile
// (and an ICameraDevice subclass only if their transport isn't already covered).
struct DeviceProfile
{
    uint16_t vid;
    uint16_t pid;
    TransportClass transport;
    const wchar_t* displayName;     // branding, used at MFCreateVirtualCamera
    const VideoMode* modes;         // advertised modes
    uint32_t modeCount;
    uint32_t formatMask;            // FMT_* the DLL may advertise for this device
    VideoMode defaultMode;
    uint32_t defaultFormat;         // FMT_NV12 (PS3 Eye) or FMT_YUY2 (EyeToy)
    bool hasLed;
    uint32_t controlMask;           // CTRL_* the Settings dialog should enable
};

// True if `p` advertises a capture mode exactly matching (w,h,fps). Shared by
// the host's initial mode pick and its hot-plug re-advertise so both agree on
// what a device can serve.
inline bool ProfileHasMode(const DeviceProfile& p, uint32_t w, uint32_t h, uint32_t fps)
{
    for (uint32_t i = 0; i < p.modeCount; ++i)
        if (p.modes[i].width == w && p.modes[i].height == h && p.modes[i].fps == fps)
            return true;
    return false;
}

// One acquired frame. Buffers are owned by the device and valid only until the
// next AcquireFrame/Stop on the same device. yuy2 is always present on success;
// jpeg is null for devices without a JFIF sidecar (PS3 Eye).
struct AcquiredFrame
{
    const uint8_t* yuy2;
    uint32_t       yuy2Bytes;
    const uint8_t* jpeg;
    uint32_t       jpegBytes;
};

class ICameraDevice
{
public:
    virtual ~ICameraDevice() = default;

    // Open USB and configure the sensor for `mode`. Returns false on failure,
    // leaving the device fully stopped (USB released).
    virtual bool Init(const VideoMode& mode) = 0;

    // Begin streaming. Returns false if transfers could not be submitted
    // (device left stopped).
    virtual bool Start() = 0;

    // Stop streaming; sensor off, LED off, 0% CPU. Idempotent.
    virtual void Stop() = 0;

    // Pull the next frame into device-owned buffers. Blocks up to timeoutMs;
    // returns false on timeout or when streaming stopped. wantJpeg lets a
    // JPEG-native device skip retaining the JFIF when no MJPEG client is active;
    // devices without a JPEG path ignore it and set out.jpeg = nullptr.
    virtual bool AcquireFrame(AcquiredFrame& out, uint32_t timeoutMs, bool wantJpeg) = 0;

    // Push sensor settings (gain/exposure/WB/flip/...). Safe live or pre-start.
    virtual void ApplySettings(const Settings& s) = 0;

    virtual const DeviceProfile& Profile() const = 0;
};
