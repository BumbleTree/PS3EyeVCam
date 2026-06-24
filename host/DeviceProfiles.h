#pragma once
//
// Static DeviceProfile table. Data, not code: CaptureController / DeviceRegistry
// / the DLL parameterise off these instead of switching on a product enum.
//
#include "ICameraDevice.h"

// The PS3 Eye profile.
const DeviceProfile& Ps3EyeProfile();

// The PS2 EyeToy profile (OV519/OV7648 over WinUSB iso).
const DeviceProfile& EyeToyProfile();

// Profile for a USB VID/PID, or nullptr if the device is not supported.
const DeviceProfile* FindDeviceProfile(uint16_t vid, uint16_t pid);
