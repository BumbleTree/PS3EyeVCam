#pragma once
//
// Identity of the PS3 Eye virtual camera media sources.
//
// CLSID_PS3EyeVCams[i] is the COM class registered by PS3EyeVCam.dll (the
// IMFActivate implementation) for camera slot i. The host passes the string
// form to MFCreateVirtualCamera as `sourceId`, and the Frame Server CoCreates
// it inside its own service process.
//
#include <guiddef.h>

// Number of supported camera slots; sized to match every array below.
inline constexpr int kVCamCount = 8;

// Arrays of 8 distinct static CLSIDs and details for multi-camera support.
inline constexpr GUID CLSID_PS3EyeVCams[kVCamCount] = {
    { 0xe5c9a2d4, 0x7b3f, 0x4c81, { 0x9a, 0x06, 0x2f, 0x51, 0xd0, 0x8b, 0x6e, 0x73 } },
    { 0xe5c9a2d5, 0x7b3f, 0x4c81, { 0x9a, 0x06, 0x2f, 0x51, 0xd0, 0x8b, 0x6e, 0x73 } },
    { 0xe5c9a2d6, 0x7b3f, 0x4c81, { 0x9a, 0x06, 0x2f, 0x51, 0xd0, 0x8b, 0x6e, 0x73 } },
    { 0xe5c9a2d7, 0x7b3f, 0x4c81, { 0x9a, 0x06, 0x2f, 0x51, 0xd0, 0x8b, 0x6e, 0x73 } },
    { 0xe5c9a2d8, 0x7b3f, 0x4c81, { 0x9a, 0x06, 0x2f, 0x51, 0xd0, 0x8b, 0x6e, 0x73 } },
    { 0xe5c9a2d9, 0x7b3f, 0x4c81, { 0x9a, 0x06, 0x2f, 0x51, 0xd0, 0x8b, 0x6e, 0x73 } },
    { 0xe5c9a2da, 0x7b3f, 0x4c81, { 0x9a, 0x06, 0x2f, 0x51, 0xd0, 0x8b, 0x6e, 0x73 } },
    { 0xe5c9a2db, 0x7b3f, 0x4c81, { 0x9a, 0x06, 0x2f, 0x51, 0xd0, 0x8b, 0x6e, 0x73 } }
};

inline constexpr const wchar_t* kVCamClsidStrings[kVCamCount] = {
    L"{E5C9A2D4-7B3F-4C81-9A06-2F51D08B6E73}",
    L"{E5C9A2D5-7B3F-4C81-9A06-2F51D08B6E73}",
    L"{E5C9A2D6-7B3F-4C81-9A06-2F51D08B6E73}",
    L"{E5C9A2D7-7B3F-4C81-9A06-2F51D08B6E73}",
    L"{E5C9A2D8-7B3F-4C81-9A06-2F51D08B6E73}",
    L"{E5C9A2D9-7B3F-4C81-9A06-2F51D08B6E73}",
    L"{E5C9A2DA-7B3F-4C81-9A06-2F51D08B6E73}",
    L"{E5C9A2DB-7B3F-4C81-9A06-2F51D08B6E73}"
};

inline constexpr const wchar_t* kVCamFriendlyNames[kVCamCount] = {
    L"PS3 Eye",
    L"PS3 Eye #1",
    L"PS3 Eye #2",
    L"PS3 Eye #3",
    L"PS3 Eye #4",
    L"PS3 Eye #5",
    L"PS3 Eye #6",
    L"PS3 Eye #7"
};
