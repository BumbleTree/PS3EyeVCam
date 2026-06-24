#include "FrameBusPreviewSource.h"

#include "../common/ControlBus.h"

bool FrameBusPreviewSource::TryOpen(int cameraIndex)
{
    _cameraIndex = cameraIndex;
    return _reader.TryOpen(cameraIndex);
}

bool FrameBusPreviewSource::IsOpen() const
{
    return _reader.IsOpen();
}

void FrameBusPreviewSource::Close()
{
    _reader.Close();
}

bool FrameBusPreviewSource::ReadFormat(uint32_t& width, uint32_t& height, uint32_t& fps)
{
    framebus::HotHeader h{};
    if (!_reader.ReadFormat(h))
        return false;
    width  = h.width;
    height = h.height;
    fps    = h.fpsNum / (h.fpsDen ? h.fpsDen : 1);
    return true;
}

void FrameBusPreviewSource::WaitFrame(DWORD timeoutMs) const
{
    // INVARIANT (docs/TDD.md §5.3): poll the seqlock, never wait on the shared
    // ".FrameReady" event. It is auto-reset, so SetEvent wakes exactly ONE
    // waiter; the single permitted waiter is the virtual camera DLL's
    // DeliverSample. Waiting here would steal its wakeups and starve clients
    // (OBS/Discord). Keep this a Sleep — do not convert it to an event wait.
    Sleep(timeoutMs < 16 ? timeoutMs : 16);
}

LONG64 FrameBusPreviewSource::TryReadNewer(uint8_t* dst, uint32_t dstBytes, LONG64 lastFrameId)
{
    return _reader.TryReadNewer(dst, dstBytes, lastFrameId);
}

bool FrameBusPreviewSource::IsCameraInUse(uint32_t idleTimeoutMs) const
{
    // Read-only query: opens the ControlBus section FILE_MAP_READ, reads the
    // keepalive tick once, closes. MAXULONGLONG (section absent / never
    // stamped) means no external client is consuming.
    const ULONGLONG age = controlbus::ActivityAge(_cameraIndex);
    return age != MAXULONGLONG && age < idleTimeoutMs;
}
