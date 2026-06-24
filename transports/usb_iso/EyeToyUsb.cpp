#include "EyeToyUsb.h"

#include <libusb.h>
#include <cstdio>

namespace eyetoy {

bool IsEyeToy(unsigned short vid, unsigned short pid)
{
    return vid == 0x054C && (pid == 0x0154 || pid == 0x0155);
}

std::string PortPath(libusb_device* dev)
{
    uint8_t ports[8];
    const int n = libusb_get_port_numbers(dev, ports, sizeof(ports));
    char buf[64];
    int off = snprintf(buf, sizeof(buf), "%d", libusb_get_bus_number(dev));
    for (int i = 0; i < n && off > 0 && off < (int)sizeof(buf); ++i)
        off += snprintf(buf + off, sizeof(buf) - off, ".%d", ports[i]);
    return std::string(buf);
}

std::vector<std::string> EnumeratePortPaths()
{
    std::vector<std::string> paths;
    libusb_context* ctx = UsbContext::Instance().Get();
    if (!ctx)
        return paths;
    libusb_device** list = nullptr;
    const ssize_t cnt = libusb_get_device_list(ctx, &list);
    for (ssize_t i = 0; i < cnt; ++i)
    {
        libusb_device_descriptor dd{};
        if (libusb_get_device_descriptor(list[i], &dd) != 0)
            continue;
        if (IsEyeToy(dd.idVendor, dd.idProduct))
            paths.push_back(PortPath(list[i]));
    }
    if (list)
        libusb_free_device_list(list, 1);
    return paths;
}

libusb_device_handle* OpenByPortPath(const std::string& portPath)
{
    libusb_context* ctx = UsbContext::Instance().Get();
    if (!ctx)
        return nullptr;
    libusb_device** list = nullptr;
    const ssize_t cnt = libusb_get_device_list(ctx, &list);
    libusb_device_handle* h = nullptr;
    for (ssize_t i = 0; i < cnt; ++i)
    {
        libusb_device_descriptor dd{};
        if (libusb_get_device_descriptor(list[i], &dd) != 0)
            continue;
        if (!IsEyeToy(dd.idVendor, dd.idProduct))
            continue;
        if (!portPath.empty() && PortPath(list[i]) != portPath)
            continue;
        if (libusb_open(list[i], &h) != 0)
            h = nullptr;
        break;
    }
    if (list)
        libusb_free_device_list(list, 1);
    return h;
}


UsbContext& UsbContext::Instance()
{
    // Thread-safe C++11 magic-static init; lives for the process.
    static UsbContext sInstance;
    return sInstance;
}

UsbContext::UsbContext()
{
    libusb_init(&_ctx);
}

UsbContext::~UsbContext()
{
    // Best-effort: if a device leaked a reference the thread is still running.
    if (_thread.joinable())
    {
        _exit.store(true, std::memory_order_release);
        _thread.join();
    }
    if (_ctx)
        libusb_exit(_ctx);
}

void UsbContext::Acquire()
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (_users++ == 0)
    {
        _exit.store(false, std::memory_order_release);
        _thread = std::thread(&UsbContext::EventThreadFunc, this);
    }
}

void UsbContext::Release()
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (_users > 0 && --_users == 0)
    {
        _exit.store(true, std::memory_order_release);
        if (_thread.joinable())
            _thread.join();
    }
}

void UsbContext::EventThreadFunc()
{
    // Pump libusb events on a 50 ms timeout (same cadence as ps3eye's transfer
    // thread). The timeout bounds shutdown latency without busy-spinning; iso
    // completion callbacks fire from here.
    while (!_exit.load(std::memory_order_acquire))
    {
        timeval tv{ 0, 50 * 1000 };
        libusb_handle_events_timeout_completed(_ctx, &tv, nullptr);
    }
}

} // namespace eyetoy
