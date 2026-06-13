#pragma once
//
// Per-camera kernel object / registry name formatting, shared by every layer
// (FrameBus, ControlBus, Settings). Index 0 keeps the legacy un-numbered names
// ("Global\PS3EyeVCam.FrameBus"); other indices embed the digit
// ("Global\PS3EyeVCam3.FrameBus").
//
#include <wchar.h>
#include <cstdio>

namespace ipcnames {

template <size_t N>
inline void Format(wchar_t (&buf)[N], const wchar_t* suffix, int cameraIndex)
{
    if (cameraIndex == 0)
        swprintf_s(buf, L"Global\\PS3EyeVCam%s", suffix);
    else
        swprintf_s(buf, L"Global\\PS3EyeVCam%d%s", cameraIndex, suffix);
}

} // namespace ipcnames
