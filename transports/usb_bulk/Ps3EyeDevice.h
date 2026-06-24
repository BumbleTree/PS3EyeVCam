#pragma once
//
// Ps3EyeDevice — ICameraDevice over the vendored PS3EYEDriver (WinUSB bulk,
// ctx #1). Thin, behaviour-preserving wrapper: the fused Bayer->YUY2 debayer,
// the sub-millisecond bulk path, and the sensor control writes are all the
// existing PS3EYECam code, unchanged. The DeviceRegistry injects the PS3EYERef.
//
#include <memory>
#include "../../host/ICameraDevice.h"
#include "ps3eye.h"

class Ps3EyeDevice : public ICameraDevice
{
public:
    explicit Ps3EyeDevice(ps3eye::PS3EYECam::PS3EYERef eye);

    bool Init(const VideoMode& mode) override;
    bool Start() override;
    void Stop() override;
    bool AcquireFrame(AcquiredFrame& out, uint32_t timeoutMs, bool wantJpeg) override;
    void ApplySettings(const Settings& s) override;
    const DeviceProfile& Profile() const override;

private:
    ps3eye::PS3EYECam::PS3EYERef _eye;
    std::unique_ptr<uint8_t[]>   _buf;   // getFrame writes publish-ready YUY2 here
    uint32_t _width  = 0;
    uint32_t _height = 0;
};
