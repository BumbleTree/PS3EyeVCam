#include "DeviceProfiles.h"

// PS3 Eye: all 13 native modes (Settings.h kVideoModes); serves YUY2 + NV12;
// default NV12 640x480@60 (unchanged from the current DLL default). No LED hook
// (the PS3 Eye LED follows USB power, not a software GPIO).
static const DeviceProfile kPs3EyeProfile = {
    0x1415, 0x2000,
    TransportClass::UsbBulk_Libusb,
    L"PS3 Eye",
    kVideoModes, static_cast<uint32_t>(kVideoModeCount),
    FMT_YUY2 | FMT_NV12,
    { 640, 480, 60 },
    FMT_NV12,
    false,
    // Ps3EyeDevice backs flip, auto+manual gain/exposure, AWB + manual R/G/B,
    // and the test pattern. It does NOT wire brightness/saturation (greyed).
    CTRL_FLIP | CTRL_GAIN | CTRL_WHITEBAL | CTRL_WB_MANUAL | CTRL_TESTPATTERN,
};

const DeviceProfile& Ps3EyeProfile() { return kPs3EyeProfile; }

// PS2 EyeToy (OV519 bridge + OV7648). Streams JFIF over WinUSB iso; serves YUY2
// (decoded) + MJPEG (passthrough). Default YUY2 320x240@30 — the PCSX2-verified
// path. Software LED (OV519 GPIO 0x71).
static const VideoMode kEyeToyModes[] = {
    { 320, 240, 30 },   // default
    { 320, 240, 15 },
    { 640, 480, 15 },
};
static const DeviceProfile kEyeToyProfile = {
    0x054C, 0x0154,
    TransportClass::UsbIso_Libusb,
    L"PS2 EyeToy",
    kEyeToyModes, static_cast<uint32_t>(sizeof(kEyeToyModes) / sizeof(kEyeToyModes[0])),
    FMT_YUY2 | FMT_MJPEG,
    { 320, 240, 30 },
    FMT_YUY2,
    true,
    // OV7648 controls: brightness (I2C 0x06) and saturation (0x03) via the
    // proven gspca ov519 path, plus FLIP done in software during the YUY2 repack
    // (the OV7648 has no hardware flip register; EyeToyDevice mirrors the decoded
    // frame on the CPU — negligible at QVGA/VGA). Gain/exposure stay on the
    // sensor's auto defaults (gspca exposes neither for the OV7648). The AWB
    // toggle is omitted on purpose: the OV7648 has no manual R/G/B gain
    // registers, so toggling AWB off would unlock nothing (only freeze white
    // balance with no correction). AWB is left permanently on (bring-up default).
    CTRL_FLIP | CTRL_BRIGHTNESS | CTRL_SATURATION,
};

const DeviceProfile& EyeToyProfile() { return kEyeToyProfile; }

const DeviceProfile* FindDeviceProfile(uint16_t vid, uint16_t pid)
{
    if (vid == 0x1415 && pid == 0x2000) return &kPs3EyeProfile;
    if (vid == 0x054C && (pid == 0x0154 || pid == 0x0155)) return &kEyeToyProfile;
    return nullptr;
}
