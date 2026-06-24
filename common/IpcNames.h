#pragma once
//
// Per-camera kernel object name formatting, shared by every layer (FrameBus,
// ControlBus). Every slot embeds its index, so the names are uniformly
// "Global\PSCam4Win{N}.{suffix}" (e.g. "Global\PSCam4Win0.FrameBus").
//
#include <wchar.h>
#include <cstdio>

namespace ipcnames {

template <size_t N>
inline void Format(wchar_t (&buf)[N], const wchar_t* suffix, int cameraIndex)
{
    swprintf_s(buf, L"Global\\PSCam4Win%d%s", cameraIndex, suffix);
}

} // namespace ipcnames
