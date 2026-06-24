#include "EyeToyDevice.h"

#include <libusb.h>
#include <new>
#include <chrono>
#include <thread>

#include "EyeToyUsb.h"
#include "../../host/DeviceProfiles.h"
#include "../../common/FrameBus.h"   // framebus::kMaxJpegBytes, Yuy2Bytes
#include "../../third_party/libjpeg-turbo/include/turbojpeg.h"

namespace {

// iso ring parameters — promoted verbatim from tools/eyetoy_probe.cpp, which
// validated sustained streaming on hardware (EP 0x81, alt 4).
constexpr uint8_t kIsoEp      = 0x81;
constexpr int     kIsoPkts    = 32;
constexpr int     kIsoPktSize = 896;
constexpr int     kIsoRing    = 8;

void SleepMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

// ---------------------------------------------------------------------------
// OV519 + OV7648 register bring-up — ported VERBATIM from the Linux gspca
// ov519 driver via tools/eyetoy_probe.cpp (proven on hardware). reg_w/reg_r/
// i2c primitives + the QVGA/VGA mode tables.
// ---------------------------------------------------------------------------

int RegW(libusb_device_handle* h, uint16_t index, uint8_t value)
{
    uint8_t buf = value;
    return libusb_control_transfer(h, 0x40, 0x01, 0x0000, index, &buf, 1, 500);
}
int RegR(libusb_device_handle* h, uint16_t index)
{
    uint8_t buf = 0;
    int r = libusb_control_transfer(h, 0xC0, 0x01, 0x0000, index, &buf, 1, 500);
    return r < 0 ? r : buf;
}
int RegR8(libusb_device_handle* h, uint16_t index)
{
    uint8_t buf[8] = { 0 };
    int r = libusb_control_transfer(h, 0xC0, 0x01, 0x0000, index, buf, 8, 500);
    return r < 0 ? r : buf[0];
}
void RegWMask(libusb_device_handle* h, uint16_t index, uint8_t value, uint8_t mask)
{
    if (mask != 0xff)
    {
        value &= mask;
        int old = RegR(h, index);
        if (old < 0) return;
        value |= (uint8_t(old) & ~mask);
    }
    RegW(h, index, value);
}
void SetSlaveIds(libusb_device_handle* h, uint8_t slave)
{
    RegW(h, 0x41, slave);
    RegW(h, 0x44, slave + 1);
}
void I2cW(libusb_device_handle* h, uint8_t reg, uint8_t value)
{
    RegW(h, 0x42, reg);
    RegW(h, 0x45, value);
    RegW(h, 0x47, 0x01);
    SleepMs(4);
    RegR8(h, 0x47);
}
int I2cR(libusb_device_handle* h, uint8_t reg)
{
    RegW(h, 0x43, reg);
    RegW(h, 0x47, 0x03);
    RegR8(h, 0x47);
    RegW(h, 0x47, 0x05);
    RegR8(h, 0x47);
    return RegR(h, 0x45);
}
void I2cWMask(libusb_device_handle* h, uint8_t reg, uint8_t value, uint8_t mask)
{
    if (mask != 0xff)
    {
        value &= mask;
        int old = I2cR(h, reg);
        if (old < 0) return;
        value |= (uint8_t(old) & ~mask);
    }
    I2cW(h, reg, value);
}

void BringUpOv519(libusb_device_handle* h, int width, int height, int fps)
{
    const bool qvga = (width <= 320);

    static const uint8_t init519[][2] = {
        {0x5a,0x6d},{0x53,0x9b},{0x54,0xff},{0x5d,0x03},{0x49,0x01},{0x48,0x00},
        {0x72,0xee},{0x51,0x0f},{0x51,0x00},{0x22,0x00},
    };
    for (auto& rv : init519) RegW(h, rv[0], rv[1]);

    SetSlaveIds(h, 0x42);
    I2cW(h, 0x12, 0x80);
    SleepMs(150);
    (void)I2cR(h, 0x1c); (void)I2cR(h, 0x1d);   // mfg id read (sync), result unused

    I2cW(h, 0x12, 0x80);
    I2cW(h, 0x12, 0x14);

    static const uint8_t mode519[][2] = {
        {0x5d,0x03},{0x53,0x9f},{0x54,0x0f},{0xa2,0x20},{0xa3,0x18},{0xa4,0x04},{0xa5,0x28},
        {0x37,0x00},{0x55,0x02},{0x22,0x1d},{0x17,0x50},{0x37,0x00},{0x40,0xff},{0x46,0x00},
        {0x59,0x04},{0xff,0x00},
    };
    for (auto& rv : mode519) RegW(h, rv[0], rv[1]);
    RegWMask(h, 0x20, 0x10, 0x10);
    RegW(h, 0x10, uint8_t(width >> 4));
    RegW(h, 0x11, uint8_t(height >> 3));
    RegW(h, 0x12, qvga ? 0x01 : 0x00);
    RegW(h, 0x13, 0x00); RegW(h, 0x14, 0x00); RegW(h, 0x15, 0x00); RegW(h, 0x16, 0x00);
    RegW(h, 0x25, 0x03);
    RegW(h, 0x26, 0x00);
    if (fps >= 30) { RegW(h, 0xa4, 0x0c); RegW(h, 0x23, 0xff); }
    else           { RegW(h, 0xa4, 0x04); RegW(h, 0x23, 0xff); }

    I2cWMask(h, 0x14, qvga ? 0x20 : 0x00, 0x20);
    I2cWMask(h, 0x28, qvga ? 0x00 : 0x20, 0x20);
    I2cWMask(h, 0x2d, qvga ? 0x40 : 0x00, 0x40);
    I2cWMask(h, 0x67, qvga ? 0xf0 : 0x90, 0xf0);
    I2cWMask(h, 0x74, qvga ? 0x20 : 0x00, 0x20);
    I2cWMask(h, 0x12, 0x04, 0x04);
    const int hwscale = qvga ? 1 : 2;
    const int vwscale = qvga ? 0 : 1;
    I2cW(h, 0x17, 0x1a);
    I2cW(h, 0x18, uint8_t(0x1a + (width  >> hwscale)));
    I2cW(h, 0x19, 0x03);
    I2cW(h, 0x1a, uint8_t(0x03 + (height >> vwscale)));

    RegW(h, 0x51, 0x0f);
    RegW(h, 0x51, 0x00);
    RegW(h, 0x22, 0x1d);
}

void LedOn(libusb_device_handle* h)  { RegWMask(h, 0x71, 1, 1); }
void LedOff(libusb_device_handle* h) { RegWMask(h, 0x71, 0, 1); }

void StopSensor(libusb_device_handle* h)
{
    RegW(h, 0x51, 0x0f); RegW(h, 0x51, 0x00); RegW(h, 0x22, 0x00);
}

void FillYuy2Black(uint8_t* dst, uint32_t w, uint32_t h)
{
    uint32_t* p = reinterpret_cast<uint32_t*>(dst);
    const size_t words = static_cast<size_t>(w) * h / 2;
    for (size_t i = 0; i < words; ++i) p[i] = 0x80108010u;  // Y=0x10 U/V=0x80
}

} // namespace

// ---------------------------------------------------------------------------

EyeToyDevice::~EyeToyDevice()
{
    Stop();
}

bool EyeToyDevice::Init(const VideoMode& mode)
{
    _width  = mode.width;
    _height = mode.height;
    _fps    = mode.fps;

    if (!eyetoy::UsbContext::Instance().Get())
        return false;

    // Open the specific EyeToy bound to this slot's port path (empty = first
    // found). The DeviceRegistry assigns the port path per slot so two EyeToys
    // map to stable, distinct virtual cameras.
    _h = eyetoy::OpenByPortPath(_portPath);
    if (!_h)
        return false;

    libusb_set_auto_detach_kernel_driver(_h, 1);
    if (libusb_claim_interface(_h, 0) != 0)
    {
        libusb_close(_h);
        _h = nullptr;
        return false;
    }

    // Synchronous OV519/OV7648 bring-up (control transfers, internal libusb
    // pump) BEFORE the async event thread starts — avoids sync/async contention.
    BringUpOv519(_h, static_cast<int>(_width), static_cast<int>(_height), static_cast<int>(_fps));
    libusb_set_interface_alt_setting(_h, 0, 4);   // 896B alt; ample for QVGA + VGA

    _yuy2Out.reset(new (std::nothrow) uint8_t[framebus::Yuy2Bytes(_width, _height)]);
    if (!_yuy2Out)
    {
        libusb_release_interface(_h, 0);
        libusb_close(_h);
        _h = nullptr;
        return false;
    }
    FillYuy2Black(_yuy2Out.get(), _width, _height);

    _tjDecomp = tjInitDecompress();
    if (!_tjDecomp)
    {
        _yuy2Out.reset();
        libusb_release_interface(_h, 0);
        libusb_close(_h);
        _h = nullptr;
        return false;
    }
    return true;
}

bool EyeToyDevice::Start()
{
    if (!_h || _streaming)
        return false;

    // Start the event thread first so iso completions are pumped.
    eyetoy::UsbContext::Instance().Acquire();
    _ctxAcquired = true;
    _cancelling.store(false, std::memory_order_release);
    _inFrame = false;
    _reasm.clear();

    _xfers.clear();
    _xferBufs.assign(kIsoRing, {});
    for (int i = 0; i < kIsoRing; ++i)
    {
        libusb_transfer* t = libusb_alloc_transfer(kIsoPkts);
        if (!t)
            break;
        _xferBufs[i].resize(static_cast<size_t>(kIsoPkts) * kIsoPktSize);
        libusb_fill_iso_transfer(t, _h, kIsoEp, _xferBufs[i].data(),
                                 static_cast<int>(_xferBufs[i].size()),
                                 kIsoPkts, &EyeToyDevice::OnTransfer, this, 1000);
        libusb_set_iso_packet_lengths(t, kIsoPktSize);
        // Count before submit: the event thread is already pumping, so a
        // completion callback can fire (and decrement) the instant we submit.
        _xfersInFlight.fetch_add(1, std::memory_order_acq_rel);
        if (libusb_submit_transfer(t) != 0)
        {
            _xfersInFlight.fetch_sub(1, std::memory_order_acq_rel);
            libusb_free_transfer(t);
            break;
        }
        _xfers.push_back(t);
    }

    if (_xfers.empty())
    {
        Stop();
        return false;
    }
    LedOn(_h);
    _streaming = true;
    return true;
}

void EyeToyDevice::Stop()
{
    if (_h && _streaming)
    {
        // Cancel the ring and let the event thread drive every transfer to a
        // terminal (cancelled) callback before we free anything.
        _cancelling.store(true, std::memory_order_release);
        for (auto* t : _xfers)
            libusb_cancel_transfer(t);

        // The callback stops resubmitting once _cancelling is set; wait until
        // every transfer has reached its terminal callback. Bounded so a wedged
        // transfer can't hang teardown forever.
        for (int i = 0; i < 200; ++i)   // ~1s max
        {
            if (_xfersInFlight.load(std::memory_order_acquire) == 0)
                break;
            SleepMs(5);
        }
        _streaming = false;
    }

    FreeTransfers();

    if (_ctxAcquired)
    {
        eyetoy::UsbContext::Instance().Release();   // joins the event thread
        _ctxAcquired = false;
    }

    if (_h)
    {
        // Sensor off, LED off, back to the zero-bandwidth alt setting.
        LedOff(_h);
        StopSensor(_h);
        libusb_set_interface_alt_setting(_h, 0, 0);
        libusb_release_interface(_h, 0);
        libusb_close(_h);
        _h = nullptr;
    }
    if (_tjDecomp)
    {
        tjDestroy(_tjDecomp);
        _tjDecomp = nullptr;
    }
    _yuy2Out.reset();
}

void EyeToyDevice::FreeTransfers()
{
    for (auto* t : _xfers)
        if (t) libusb_free_transfer(t);
    _xfers.clear();
    _xferBufs.clear();
    _xfersInFlight.store(0, std::memory_order_release);
}

void EyeToyDevice::OnTransfer(libusb_transfer* xfer)
{
    static_cast<EyeToyDevice*>(xfer->user_data)->HandleTransfer(xfer);
}

void EyeToyDevice::HandleTransfer(libusb_transfer* xfer)
{
    if (xfer->status == LIBUSB_TRANSFER_COMPLETED ||
        xfer->status == LIBUSB_TRANSFER_TIMED_OUT)
    {
        for (int i = 0; i < xfer->num_iso_packets; ++i)
        {
            const libusb_iso_packet_descriptor& p = xfer->iso_packet_desc[i];
            if (p.status != LIBUSB_TRANSFER_COMPLETED || p.actual_length == 0)
                continue;
            const uint8_t* d = libusb_get_iso_packet_buffer_simple(xfer, i);
            if (!d)
                continue;
            const uint32_t len = p.actual_length;

            // OV519 framing: a packet beginning FF FF FF 0x50 is start-of-frame
            // (16-byte header precedes the JFIF), FF FF FF 0x51 is end-of-frame;
            // everything else is intermediate JFIF data.
            if (len >= 4 && d[0] == 0xFF && d[1] == 0xFF && d[2] == 0xFF &&
                (d[3] == 0x50 || d[3] == 0x51))
            {
                if (d[3] == 0x50)   // SOF
                {
                    _reasm.clear();
                    _inFrame = true;
                    if (len > 16)
                        _reasm.insert(_reasm.end(), d + 16, d + len);
                }
                else                // EOF
                {
                    if (_inFrame && !_reasm.empty() &&
                        _reasm.size() <= framebus::kMaxJpegBytes)
                    {
                        // Trim trailing iso zero-padding so the buffer ends
                        // exactly at the JPEG EOI (FF D9): the OV519 pads the
                        // final iso packet past EOI.
                        size_t n = _reasm.size();
                        while (n >= 2 && !(_reasm[n - 2] == 0xFF && _reasm[n - 1] == 0xD9))
                            --n;
                        if (n >= 2)
                            OnFrameComplete(_reasm.data(), static_cast<uint32_t>(n));
                    }
                    _inFrame = false;
                }
            }
            else if (_inFrame)
            {
                if (_reasm.size() + len <= framebus::kMaxJpegBytes)
                    _reasm.insert(_reasm.end(), d, d + len);
                else
                    _inFrame = false;   // overflow guard: drop the runaway frame
            }
        }
    }

    // Resubmit unless we're tearing down; track in-flight count so Stop knows
    // when the ring has drained.
    if (!_cancelling.load(std::memory_order_acquire) &&
        xfer->status != LIBUSB_TRANSFER_NO_DEVICE)
    {
        if (libusb_submit_transfer(xfer) == 0)
            return;   // still in flight
    }
    _xfersInFlight.fetch_sub(1, std::memory_order_acq_rel);  // terminal for this transfer
}

void EyeToyDevice::OnFrameComplete(const uint8_t* jpeg, uint32_t len)
{
    std::lock_guard<std::mutex> lk(_mbMutex);
    _mbJpeg.assign(jpeg, jpeg + len);
    ++_mbSeq;
    _mbCv.notify_one();
}

bool EyeToyDevice::AcquireFrame(AcquiredFrame& out, uint32_t timeoutMs, bool wantJpeg)
{
    std::vector<uint8_t> jpeg;
    {
        std::unique_lock<std::mutex> lk(_mbMutex);
        if (!_mbCv.wait_for(lk, std::chrono::milliseconds(timeoutMs),
                            [&] { return _mbSeq != _consumedSeq; }))
            return false;   // timeout: no new frame
        _consumedSeq = _mbSeq;
        jpeg.swap(_mbJpeg);   // take ownership; producer reallocates next frame
    }

    // Decode JFIF -> YUY2 for the (canonical) YUY2/NV12 clients. On decode
    // failure _yuy2Out keeps the previous frame rather than flashing black.
    if (!jpeg.empty())
        DecodeJpegToYuy2(jpeg.data(), static_cast<uint32_t>(jpeg.size()));
    out.yuy2      = _yuy2Out.get();
    out.yuy2Bytes = framebus::Yuy2Bytes(_width, _height);

    if (wantJpeg && !jpeg.empty())
    {
        _jpegOut.swap(jpeg);
        out.jpeg      = _jpegOut.data();
        out.jpegBytes = static_cast<uint32_t>(_jpegOut.size());
    }
    else
    {
        out.jpeg      = nullptr;
        out.jpegBytes = 0;
    }
    return true;
}

// JFIF -> YUY2. Decodes to planar YUV at the JPEG's native subsampling (the
// EyeToy is YUV 4:2:2, a lossless repack to YUY2 with no chroma resampling),
// then interleaves Y/U/V into YUY2. Subsampling-agnostic: chroma is sampled at
// the plane's sub-rate, so 4:2:0 etc. also work. On any failure the previous
// _yuy2Out is left intact.
void EyeToyDevice::DecodeJpegToYuy2(const uint8_t* jpeg, uint32_t len)
{
    if (!_tjDecomp)
        return;
    int jw = 0, jh = 0, jss = 0, jcs = 0;
    if (tjDecompressHeader3(_tjDecomp, jpeg, len, &jw, &jh, &jss, &jcs) != 0)
        return;
    if (static_cast<uint32_t>(jw) != _width || static_cast<uint32_t>(jh) != _height)
        return;   // unexpected geometry; don't corrupt the output

    const unsigned long need = tjBufSizeYUV2(jw, 1, jh, jss);
    if (_yuvScratch.size() < need)
        _yuvScratch.resize(need);
    if (tjDecompressToYUV2(_tjDecomp, jpeg, len, _yuvScratch.data(), jw, 1, jh, 0) != 0)
        return;

    const int cw = tjPlaneWidth(1, jw, jss);   // chroma plane width
    const int ch = tjPlaneHeight(1, jh, jss);  // chroma plane height
    const int hs = jw / cw;                     // horizontal chroma sub-factor (1 or 2)
    const int vs = jh / ch;                     // vertical chroma sub-factor (1 or 2)

    const uint8_t* Y = _yuvScratch.data();
    const uint8_t* U = Y + static_cast<size_t>(jw) * jh;
    const uint8_t* V = U + static_cast<size_t>(cw) * ch;

    uint8_t* dst = _yuy2Out.get();
    const uint32_t w = _width, h = _height;
    const bool fh = _flipH, fv = _flipV;
    for (uint32_t y = 0; y < h; ++y)
    {
        const uint8_t* yr = Y + static_cast<size_t>(y) * w;
        const uint8_t* ur = U + static_cast<size_t>(y / vs) * cw;
        const uint8_t* vr = V + static_cast<size_t>(y / vs) * cw;
        // Vertical flip: emit this source row into the mirrored destination row
        // (just a different row base — no inner-loop cost).
        const uint32_t dy = fv ? (h - 1 - y) : y;
        uint8_t* o = dst + static_cast<size_t>(dy) * w * 2;
        if (!fh)
        {
            for (uint32_t x = 0; x < w; x += 2)
            {
                const int cx = x / hs;
                o[0] = yr[x];
                o[1] = ur[cx];
                o[2] = yr[x + 1];
                o[3] = vr[cx];
                o += 4;
            }
        }
        else
        {
            // Horizontal flip: the output macropixel covering columns
            // (w-2-x, w-1-x) takes source pixels x+1 (left) and x (right), so the
            // Y pair is swapped and the destination is walked back-to-front. The
            // shared 4:2:2 chroma sample is the same either way.
            o += static_cast<size_t>(w - 2) * 2;
            for (uint32_t x = 0; x < w; x += 2)
            {
                const int cx = x / hs;
                o[0] = yr[x + 1];
                o[1] = ur[cx];
                o[2] = yr[x];
                o[3] = vr[cx];
                o -= 4;
            }
        }
    }
}

void EyeToyDevice::ApplySettings(const Settings& s)
{
    // Flip is done in software during decode (the OV7648 has no hardware flip
    // register in the proven driver path), so latch it even if the sensor isn't
    // open yet — it takes effect on the next decoded frame.
    _flipH = s.flipH;
    _flipV = s.flipV;

    if (!_h)
        return;

    // OV7648 sensor controls, mapped exactly as the Linux gspca ov519 driver
    // does for SEN_OV7648 (the only adjustable controls it exposes for this
    // sensor):
    //   * brightness -> I2C reg 0x06 (setbrightness)
    //   * saturation -> I2C reg 0x03, high nibble only (setcolors: val & 0xf0)
    // Gain/exposure/AGC/AEC and white balance are deliberately NOT touched:
    // gspca does not expose them for the OV7648, whose auto loops (enabled by
    // the bring-up: AGC/AEC + AWB via COM7 reg 0x12) are the working default.
    // AWB in particular is left permanently on — the OV7648 has no manual R/G/B
    // gain registers, so a user-facing AWB toggle could only freeze white
    // balance with no way to correct it (see DeviceProfiles EyeToy mask).
    // CaptureController calls this pre-Start (no event thread yet — same context
    // as the bring-up) and live from the capture thread; the iso event thread
    // never touches I2C, and libusb serialises the synchronous control transfers
    // against it.
    I2cW(_h, 0x06, static_cast<uint8_t>(s.brightness));
    I2cW(_h, 0x03, static_cast<uint8_t>(s.saturation & 0xf0));
}

const DeviceProfile& EyeToyDevice::Profile() const { return EyeToyProfile(); }
