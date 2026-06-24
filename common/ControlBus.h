#pragma once
//
// ControlBus — reverse channel for camera sleep/wake coordination.
//
// The media source (DLL, inside the Frame Server as LOCAL SERVICE) stamps a
// keepalive tick whenever a client is actually consuming frames, and pulses a
// wake event so a sleeping host reacts instantly. The host streams the
// physical camera only while the tick is fresh.
//
// A timestamp (not a refcount) is used deliberately: it self-heals if the
// Frame Server crashes or leaks a stream — staleness always wins.
//
// DACL grants only SYSTEM / Administrators / LOCAL SERVICE. Unlike the frame
// section, Everyone gets nothing: a writable keepalive would let any local
// process pin the camera awake.
//
#include <windows.h>
#include <sddl.h>
#include <cstdint>
#include "IpcNames.h"

namespace controlbus {

constexpr const wchar_t* kSddl = L"D:(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;LS)";

constexpr uint32_t kMagic   = 0x50334543;  // 'P3EC'
constexpr uint32_t kVersion = 2;

// Which pixel format(s) the active MF client negotiated. The DLL knows the
// negotiated subtype only inside MediaStream::Start, so it publishes these bits
// here (the FrameBus is read-only to the DLL); the host reads them to gate
// optional per-frame work, e.g. filling the JPEG sidecar only for an MJPEG
// client. Values match host FormatBits (ICameraDevice.h) and FrameBus
// formatMask so a mask bit means the same thing across all three.
enum ConsumerFormat : uint32_t
{
    kConsumeYUY2  = 1u,
    kConsumeNV12  = 2u,
    kConsumeMJPEG = 4u,
};

#pragma pack(push, 8)
struct Header
{
    uint32_t magic;
    uint32_t version;
    volatile LONG64 lastActivityTick;  // GetTickCount64() of last client activity
    volatile uint32_t consumerMask;    // ConsumerFormat bits; 0 = no active client
    uint32_t pad0;
};
#pragma pack(pop)

// ------------------------------------------------------------------ Host ----

class Host
{
public:
    ~Host() { Close(); }

    bool Create(int cameraIndex)
    {
        wchar_t sectionName[64];
        wchar_t wakeEventName[64];
        ipcnames::Format(sectionName, L".Control", cameraIndex);
        ipcnames::Format(wakeEventName, L".Wake", cameraIndex);

        SECURITY_ATTRIBUTES sa{ sizeof(sa), nullptr, FALSE };
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
                kSddl, SDDL_REVISION_1, &sa.lpSecurityDescriptor, nullptr))
            return false;

        _map = CreateFileMappingW(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE,
                                  0, sizeof(Header), sectionName);
        if (_map)
            _wake = CreateEventW(&sa, FALSE /*auto-reset*/, FALSE, wakeEventName);
        LocalFree(sa.lpSecurityDescriptor);
        if (!_map || !_wake) { Close(); return false; }

        _view = static_cast<Header*>(MapViewOfFile(_map, FILE_MAP_ALL_ACCESS, 0, 0, 0));
        if (!_view) { Close(); return false; }

        _view->version = kVersion;
        _view->lastActivityTick = 0;
        _view->consumerMask = 0;
        MemoryBarrier();
        _view->magic = kMagic;
        return true;
    }

    // Milliseconds since a client last consumed frames (MAXULONGLONG if never).
    ULONGLONG ActivityAgeMs() const
    {
        const LONG64 tick = ReadAcquire64(&_view->lastActivityTick);
        if (tick == 0)
            return MAXULONGLONG;
        const ULONGLONG now = GetTickCount64();
        return now > static_cast<ULONGLONG>(tick) ? now - static_cast<ULONGLONG>(tick) : 0;
    }

    // ConsumerFormat bits the active client negotiated (0 = none). Plain aligned
    // 32-bit load (atomic on x64). The host uses this to decide wantJpeg etc.
    uint32_t ConsumerMask() const
    {
        return _view ? _view->consumerMask : 0;
    }

    HANDLE WakeEvent() const { return _wake; }
    void   ResetWake() const { ResetEvent(_wake); }

    void Close()
    {
        if (_view) { UnmapViewOfFile(_view); _view = nullptr; }
        if (_map)  { CloseHandle(_map); _map = nullptr; }
        if (_wake) { CloseHandle(_wake); _wake = nullptr; }
    }

private:
    HANDLE  _map = nullptr;
    HANDLE  _wake = nullptr;
    Header* _view = nullptr;
};

// ---------------------------------------------------------------- Pinger ----
// Used by the media source inside the Frame Server. No-ops gracefully while
// the host (and therefore the section/event) does not exist.

class Pinger
{
public:
    ~Pinger() { Close(); }

    bool IsOpen() const { return _view != nullptr; }

    bool TryOpen(int cameraIndex)
    {
        if (_view)
            return true;
        wchar_t sectionName[64];
        wchar_t wakeEventName[64];
        ipcnames::Format(sectionName, L".Control", cameraIndex);
        ipcnames::Format(wakeEventName, L".Wake", cameraIndex);

        _map = OpenFileMappingW(FILE_MAP_WRITE | FILE_MAP_READ, FALSE, sectionName);
        if (!_map)
            return false;
        _view = static_cast<Header*>(MapViewOfFile(_map, FILE_MAP_WRITE | FILE_MAP_READ, 0, 0, 0));
        if (!_view || _view->magic != kMagic)
        {
            Close();
            return false;
        }
        // Without the wake event a sleeping host (waiting INFINITE on it)
        // would never see this client: a keepalive stamp alone is not enough.
        // The host creates the event together with the section, so a missing
        // event means "host not ready" — fail the open and retry later.
        _wake = OpenEventW(EVENT_MODIFY_STATE, FALSE, wakeEventName);
        if (!_wake)
        {
            Close();
            return false;
        }
        return true;
    }

    // Plain aligned 64-bit store: atomic on x64. (This view is writable, but
    // stay with non-interlocked accesses on shared views as a project rule.)
    void Stamp()
    {
        if (_view)
            WriteRelease64(&_view->lastActivityTick,
                           static_cast<LONG64>(GetTickCount64()));
    }

    // Publish which pixel format(s) the active client negotiated. Plain aligned
    // 32-bit store (atomic on x64). Pass 0 to clear it when no client is active.
    void SetConsumerMask(uint32_t mask)
    {
        if (_view)
            _view->consumerMask = mask;
    }

    void SignalWake()
    {
        if (_wake)
            SetEvent(_wake);
    }

    void Close()
    {
        if (_view) { UnmapViewOfFile(_view); _view = nullptr; }
        if (_map)  { CloseHandle(_map); _map = nullptr; }
        if (_wake) { CloseHandle(_wake); _wake = nullptr; }
    }

private:
    HANDLE  _map = nullptr;
    HANDLE  _wake = nullptr;
    Header* _view = nullptr;
};

// -------------------------------------------------------- ActivityAge --------
// Read-only query for callers (the preview UI) that need to know whether a
// client is consuming frames but must not take a writable Host handle. Opens
// the ControlBus section FILE_MAP_READ, reads lastActivityTick once, and
// closes. Returns MAXULONGLONG if the section does not exist yet or has never
// been stamped. Pure leaf, safe to call from any thread.
inline ULONGLONG ActivityAge(int cameraIndex)
{
    wchar_t sectionName[64];
    ipcnames::Format(sectionName, L".Control", cameraIndex);

    HANDLE map = OpenFileMappingW(FILE_MAP_READ, FALSE, sectionName);
    if (!map)
        return MAXULONGLONG;

    Header* view = static_cast<Header*>(
        MapViewOfFile(map, FILE_MAP_READ, 0, 0, 0));
    ULONGLONG age = MAXULONGLONG;
    if (view && view->magic == kMagic)
    {
        const LONG64 tick = ReadAcquire64(&view->lastActivityTick);
        if (tick != 0)
        {
            const ULONGLONG now = GetTickCount64();
            age = now > static_cast<ULONGLONG>(tick) ? now - static_cast<ULONGLONG>(tick) : 0;
        }
    }
    if (view)
        UnmapViewOfFile(view);
    CloseHandle(map);
    return age;
}

} // namespace controlbus
