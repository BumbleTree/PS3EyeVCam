#pragma once
//
// EyeToyUsb — the EyeToy's own libusb context and event-pumping thread.
//
// Deliberately SEPARATE from the vendored PS3EYEDriver's libusb context
// (ps3eye.cpp owns its own libusb_init + transfer thread). Two reasons:
//   1. ps3eye.cpp is vendored and unmodifiable — we can't share its context.
//   2. Event-loop isolation: the EyeToy's iso completion handling never shares
//      a thread with the PS3 Eye's bulk path, so neither can stall the other.
//
// Process-lifetime singleton (magic-static). The libusb context is created once
// and lives for the process; the event thread runs only while >=1 device holds
// the context (ref-counted Acquire/Release), so an idle machine with no EyeToy
// streaming burns zero CPU here — same zero-idle discipline as the PS3 path.
//
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct libusb_context;
struct libusb_device;
struct libusb_device_handle;

namespace eyetoy {

// USB identity of the EyeToy camera interface (both PID revisions).
bool IsEyeToy(unsigned short vid, unsigned short pid);

// Stable "bus.p1.p2..." physical port-path string for a device. Same physical
// port yields the same string across replugs, so it is the slot key.
std::string PortPath(libusb_device* dev);

// Port paths of every EyeToy currently present on the shared context.
std::vector<std::string> EnumeratePortPaths();

// Open the EyeToy at `portPath` (or the first one found if empty). Caller owns
// the returned handle (libusb_close). Returns nullptr if not found / open fails.
libusb_device_handle* OpenByPortPath(const std::string& portPath);


class UsbContext
{
public:
    static UsbContext& Instance();

    libusb_context* Get() const { return _ctx; }

    // Ref-counted event-thread lifetime. The first Acquire starts the thread;
    // the last Release stops and joins it. Safe to call from multiple capture
    // threads (guarded).
    void Acquire();
    void Release();

private:
    UsbContext();
    ~UsbContext();
    UsbContext(const UsbContext&) = delete;
    UsbContext& operator=(const UsbContext&) = delete;

    void EventThreadFunc();

    libusb_context*   _ctx = nullptr;
    std::thread       _thread;
    std::atomic<bool> _exit{ false };
    std::mutex        _mutex;        // serializes user-count transitions
    int               _users = 0;
};

} // namespace eyetoy
