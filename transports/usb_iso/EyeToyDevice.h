#pragma once
//
// EyeToyDevice — ICameraDevice for the PS2 EyeToy (OmniVision OV519 bridge +
// OV7648 sensor) over WinUSB isochronous via the vendored libusb 1.0.27.
//
// Pipeline (additive; the PS3 Eye path is untouched):
//   EyeToyUsb event thread  --iso completion-->  IsoCallback (this file)
//      reassembles OV519 SOF/EOF-framed packets into a complete JFIF frame
//      --> 1-deep mailbox (overwrite-newest, lock + condvar)
//   capture thread          --AcquireFrame-->    waits the mailbox, then
//      (decode JFIF -> YUY2) and/or hands the JFIF through for MJPEG clients
//
// The mailbox keeps the iso event thread free of any heavy work and keeps the
// CaptureController the single FrameBus writer (docs/TDD.md §6.3).
//
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "../../host/ICameraDevice.h"

struct libusb_device_handle;
struct libusb_transfer;

class EyeToyDevice final : public ICameraDevice
{
public:
    // portPath selects which physical EyeToy (the DeviceRegistry slot key);
    // empty opens the first one found.
    explicit EyeToyDevice(std::string portPath = {}) : _portPath(std::move(portPath)) {}
    ~EyeToyDevice() override;

    bool Init(const VideoMode& mode) override;
    bool Start() override;
    void Stop() override;
    bool AcquireFrame(AcquiredFrame& out, uint32_t timeoutMs, bool wantJpeg) override;
    void ApplySettings(const Settings& s) override;
    const DeviceProfile& Profile() const override;

private:
    // iso completion entry (static thunk -> OnIsoPacket reassembly).
    static void OnTransfer(libusb_transfer* xfer);
    void HandleTransfer(libusb_transfer* xfer);
    void OnFrameComplete(const uint8_t* jpeg, uint32_t len);  // deliver to mailbox
    void FreeTransfers();

    std::string _portPath;        // physical port-path slot key (empty = first found)
    libusb_device_handle* _h = nullptr;
    bool _ctxAcquired = false;
    bool _streaming   = false;

    uint32_t _width = 320, _height = 240, _fps = 30;

    // Software image flip applied during the YUY2 repack (the OV7648 has no
    // hardware flip register in the proven driver path). Latched by ApplySettings
    // and read by DecodeJpegToYuy2 — both run on the capture thread, so no sync.
    bool _flipH = false, _flipV = false;

    // iso ring (allocated in Start, freed in Stop).
    std::vector<libusb_transfer*>          _xfers;
    std::vector<std::vector<uint8_t>>      _xferBufs;
    std::atomic<bool>                      _cancelling{ false };
    // Transfers still owned by libusb (submitted, not yet terminal). Bumped
    // before each submit; the callback decrements when it stops resubmitting.
    // Stop waits for this to reach 0 before freeing the ring.
    std::atomic<int>                       _xfersInFlight{ 0 };

    // Reassembly state — touched ONLY on the EyeToyUsb event thread.
    std::vector<uint8_t> _reasm;     // current frame's accumulating JFIF
    bool                 _inFrame = false;

    // 1-deep mailbox between the event thread (producer) and AcquireFrame
    // (consumer). Overwrite-newest: a slow consumer drops frames, never blocks
    // the producer.
    std::mutex              _mbMutex;
    std::condition_variable _mbCv;
    std::vector<uint8_t>    _mbJpeg;        // latest complete JFIF
    uint64_t                _mbSeq = 0;     // ++ per complete frame
    uint64_t                _consumedSeq = 0;

    // JFIF -> YUY2 decode (libjpeg-turbo, direct YUV planar -> YUY2 repack, no
    // RGB detour). Runs on the capture thread inside AcquireFrame; the EyeToy's
    // 320x240@30 is far off the PS3 sub-ms path, so a per-frame decode here is
    // ample without a separate worker.
    void  DecodeJpegToYuy2(const uint8_t* jpeg, uint32_t len);
    void* _tjDecomp = nullptr;              // tjhandle (opaque)
    std::vector<uint8_t> _yuvScratch;       // planar YUV decode target

    // Output buffers handed back through AcquiredFrame (capture-thread owned).
    std::unique_ptr<uint8_t[]> _yuy2Out;    // decoded YUY2 frame
    std::vector<uint8_t>       _jpegOut;    // JFIF copy for MJPEG passthrough
};
