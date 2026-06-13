#include "Settings.h"
#include <windows.h>
#include <cstdio>

namespace
{

bool ReadDword(HKEY key, const wchar_t* name, uint32_t* out)
{
    DWORD value = 0, size = sizeof(value), type = 0;
    if (RegQueryValueExW(key, name, nullptr, &type,
                         reinterpret_cast<BYTE*>(&value), &size) != ERROR_SUCCESS ||
        type != REG_DWORD)
        return false;
    *out = value;
    return true;
}

bool ReadBool(HKEY key, const wchar_t* name, bool* out)
{
    uint32_t v;
    if (!ReadDword(key, name, &v))
        return false;
    *out = v != 0;
    return true;
}

void WriteDword(HKEY key, const wchar_t* name, uint32_t value)
{
    RegSetValueExW(key, name, 0, REG_DWORD,
                   reinterpret_cast<const BYTE*>(&value), sizeof(value));
}

uint32_t Clamp(uint32_t v, uint32_t lo, uint32_t hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

// Camera 0 lives at the root key (legacy layout); 1..N-1 in Camera%d subkeys.
void FormatSettingsKeyPath(wchar_t (&buf)[128], int cameraIndex)
{
    if (cameraIndex == 0)
        swprintf_s(buf, L"SOFTWARE\\PS3EyeVCam");
    else
        swprintf_s(buf, L"SOFTWARE\\PS3EyeVCam\\Camera%d", cameraIndex);
}

} // namespace

namespace settings
{

Settings Defaults()
{
    return Settings{};  // struct in-class defaults are the sensor factory values
}

int FindModeIndex(uint32_t w, uint32_t h, uint32_t fps)
{
    for (int i = 0; i < kVideoModeCount; ++i)
        if (kVideoModes[i].width == w && kVideoModes[i].height == h && kVideoModes[i].fps == fps)
            return i;
    return -1;
}

Settings Load(int cameraIndex)
{
    Settings s;  // struct defaults
    HKEY key = nullptr;
    wchar_t subKeyPath[128];
    FormatSettingsKeyPath(subKeyPath, cameraIndex);

    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, subKeyPath, 0, KEY_READ, &key) != ERROR_SUCCESS)
        return s;

    ReadDword(key, L"Width",    &s.width);
    ReadDword(key, L"Height",   &s.height);
    ReadDword(key, L"Fps",      &s.fps);
    ReadBool (key, L"FlipH",    &s.flipH);
    ReadBool (key, L"FlipV",    &s.flipV);
    ReadBool (key, L"AutoGain", &s.autoGain);
    ReadDword(key, L"Gain",     &s.gain);
    ReadDword(key, L"Exposure", &s.exposure);
    ReadBool (key, L"AutoWhiteBalance", &s.autoWhiteBalance);
    ReadDword(key, L"IdleTimeoutMs",    &s.idleTimeoutMs);

    ReadDword(key, L"RedBalance",   &s.redBalance);
    ReadDword(key, L"BlueBalance",  &s.blueBalance);
    ReadDword(key, L"GreenBalance", &s.greenBalance);
    // Legacy Hue key ignored — old builds wrote AWB blue gain (reg 0x01) here.
    ReadBool (key, L"TestPattern",  &s.testPattern);

    RegCloseKey(key);

    // Sanitize: unknown mode -> default; ranges clamped.
    if (FindModeIndex(s.width, s.height, s.fps) < 0)
    {
        const VideoMode& def = kVideoModes[kDefaultModeIndex];
        s.width = def.width; s.height = def.height; s.fps = def.fps;
    }
    s.gain     = Clamp(s.gain, 0, 63);
    s.exposure = Clamp(s.exposure, 0, 255);
    s.idleTimeoutMs = Clamp(s.idleTimeoutMs, 1000, 60000);
    s.redBalance = Clamp(s.redBalance, 0, 255);
    s.blueBalance = Clamp(s.blueBalance, 0, 255);
    s.greenBalance = Clamp(s.greenBalance, 0, 255);

    return s;
}

bool Save(int cameraIndex, const Settings& s)
{
    HKEY key = nullptr;
    wchar_t subKeyPath[128];
    FormatSettingsKeyPath(subKeyPath, cameraIndex);

    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, subKeyPath, 0, nullptr, 0,
                        KEY_WRITE, nullptr, &key, nullptr) != ERROR_SUCCESS)
        return false;
    WriteDword(key, L"Width",    s.width);
    WriteDword(key, L"Height",   s.height);
    WriteDword(key, L"Fps",      s.fps);
    WriteDword(key, L"FlipH",    s.flipH ? 1 : 0);
    WriteDword(key, L"FlipV",    s.flipV ? 1 : 0);
    WriteDword(key, L"AutoGain", s.autoGain ? 1 : 0);
    WriteDword(key, L"Gain",     s.gain);
    WriteDword(key, L"Exposure", s.exposure);
    WriteDword(key, L"AutoWhiteBalance", s.autoWhiteBalance ? 1 : 0);
    WriteDword(key, L"IdleTimeoutMs",    s.idleTimeoutMs);

    WriteDword(key, L"RedBalance",   s.redBalance);
    WriteDword(key, L"BlueBalance",  s.blueBalance);
    WriteDword(key, L"GreenBalance", s.greenBalance);
    WriteDword(key, L"TestPattern",  s.testPattern ? 1 : 0);

    RegCloseKey(key);
    return true;
}

void SeedDefaults(int cameraIndex)
{
    // Load() falls back to defaults for anything missing; writing the merge
    // back persists defaults without clobbering existing user values.
    Save(cameraIndex, Load(cameraIndex));
}

} // namespace settings
