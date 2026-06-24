#include "Ps3EyeDevice.h"

#include <new>
#include "../../host/DeviceProfiles.h"
#include "../../common/FrameBus.h"   // framebus::kMaxFrameBytes, Yuy2Bytes

using ps3eye::PS3EYECam;

Ps3EyeDevice::Ps3EyeDevice(PS3EYECam::PS3EYERef eye) : _eye(std::move(eye)) {}

bool Ps3EyeDevice::Init(const VideoMode& mode)
{
    if (!_eye)
        return false;
    _width  = mode.width;
    _height = mode.height;
    if (!_buf)
    {
        _buf.reset(new (std::nothrow) uint8_t[framebus::kMaxFrameBytes]);
        if (!_buf)
            return false;
    }
    // Native fused Bayer->YUY2 (no RGB intermediate), exactly as before.
    if (!_eye->init(mode.width, mode.height, static_cast<uint16_t>(mode.fps),
                    PS3EYECam::EOutputFormat::YUY2))
    {
        _eye->release();   // close the USB handle; don't hold it while idle
        return false;
    }
    return true;
}

bool Ps3EyeDevice::Start()
{
    if (!_eye)
        return false;
    if (!_eye->start())
    {
        _eye->release();
        return false;
    }
    return true;
}

void Ps3EyeDevice::Stop()
{
    if (_eye)
        _eye->stop();   // sensor off, LED off, transfers cancelled, interface released
}

bool Ps3EyeDevice::AcquireFrame(AcquiredFrame& out, uint32_t timeoutMs, bool /*wantJpeg*/)
{
    if (!_eye || !_buf)
        return false;
    if (!_eye->getFrame(_buf.get(), timeoutMs))
        return false;
    out.yuy2      = _buf.get();
    out.yuy2Bytes = framebus::Yuy2Bytes(_width, _height);
    out.jpeg      = nullptr;   // PS3 Eye has no JPEG sidecar
    out.jpegBytes = 0;
    return true;
}

// Push every sensor-level setting (order-safe live or pre-start). This is the
// former CaptureController::ApplySensorSettings, moved to where it belongs.
void Ps3EyeDevice::ApplySettings(const Settings& s)
{
    if (!_eye)
        return;
    _eye->setFlip(s.flipH, s.flipV);
    _eye->setAutoWhiteBalance(s.autoWhiteBalance);
    if (s.autoGain)
    {
        _eye->setAutogain(true);
    }
    else
    {
        _eye->setAutogain(false);  // also re-applies cached gain/exposure
        _eye->setGain(static_cast<uint8_t>(s.gain));
        _eye->setExposure(static_cast<uint8_t>(s.exposure));
    }
    if (!s.autoWhiteBalance)
    {
        _eye->setRedBalance(static_cast<uint8_t>(s.redBalance));
        _eye->setBlueBalance(static_cast<uint8_t>(s.blueBalance));
        _eye->setGreenBalance(static_cast<uint8_t>(s.greenBalance));
    }
    _eye->setTestPattern(s.testPattern);
}

const DeviceProfile& Ps3EyeDevice::Profile() const { return Ps3EyeProfile(); }
