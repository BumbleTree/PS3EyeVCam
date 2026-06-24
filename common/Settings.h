#pragma once
//
// Persistent configuration, stored under HKLM\SOFTWARE\PSCam4Win so it is
// writable by the (elevated) tray app and readable everywhere. The FrameBus
// header remains the DLL's source of truth for the active format; the
// registry is the *host's* source of truth across restarts.
//
#include <cstdint>

struct VideoMode
{
    uint32_t width;
    uint32_t height;
    uint32_t fps;
};

// Native OV7720/OV534 modes (valid-video rates only, matching the sensor
// tables in ps3eye.cpp).
inline constexpr VideoMode kVideoModes[] = {
    { 640, 480,  75 },
    { 640, 480,  60 },   // default
    { 640, 480,  50 },
    { 640, 480,  40 },
    { 640, 480,  30 },
    { 640, 480,  15 },
    { 320, 240, 187 },
    { 320, 240, 150 },
    { 320, 240, 125 },
    { 320, 240, 100 },
    { 320, 240,  75 },
    { 320, 240,  60 },
    { 320, 240,  30 },
};
inline constexpr int kVideoModeCount = static_cast<int>(sizeof(kVideoModes) / sizeof(kVideoModes[0]));
inline constexpr int kDefaultModeIndex = 1;  // 640x480 @ 60

struct Settings
{
    uint32_t width    = 640;
    uint32_t height   = 480;
    uint32_t fps      = 60;
    bool     flipH    = false;
    bool     flipV    = false;
    bool     autoGain = true;     // also drives auto-exposure (AGC/AEC)
    uint32_t gain     = 20;      // 0..63, used when autoGain == false
    uint32_t exposure = 120;     // 0..255, used when autoGain == false
    bool     autoWhiteBalance = true;
    uint32_t idleTimeoutMs    = 3000;

    uint32_t redBalance = 128;    // 0..255  -> reg 0x02 AWB red gain (manual WB)
    uint32_t blueBalance = 128;   // 0..255  -> reg 0x01 AWB blue gain (manual WB)
    uint32_t greenBalance = 128;  // 0..255  -> reg 0x03 AWB green gain (manual WB)
    bool     testPattern = false;

    // EyeToy (OV7648) sensor controls. The PS3 Eye ignores these; the EyeToy
    // ignores gain/exposure/flip/balance/testPattern (its OV7648 only exposes
    // brightness/saturation/AWB via the proven gspca ov519 path).
    uint32_t brightness = 127;    // 0..255  -> OV7648 reg 0x06 (Y brightness)
    uint32_t saturation = 127;    // 0..255  -> OV7648 reg 0x03 (saturation, hi nibble)

    bool SameMode(const Settings& o) const
    {
        return width == o.width && height == o.height && fps == o.fps;
    }
};

namespace settings
{
    Settings Defaults();                                   // factory defaults for one camera
    Settings Load(int cameraIndex = 0);                    // missing values -> struct defaults
    bool     Save(int cameraIndex, const Settings& s);   // full write
    void     SeedDefaults(int cameraIndex = 0);            // write only values not yet present
    int      FindModeIndex(uint32_t w, uint32_t h, uint32_t fps);  // -1 if unknown
}
