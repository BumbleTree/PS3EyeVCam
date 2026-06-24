#pragma once
//
// ICameraPreviewSource — the future-proofing seam between the Settings dialog
// preview UI and whatever transport a given camera driver uses to publish
// frames to the tray.
//
// The preview window speaks only to this interface. The implementation is
// FrameBusPreviewSource, which wraps the FrameBus seqlock shared memory + the
// read-only ControlBus activity query. It is transport-agnostic — every device
// (PS3 Eye, EyeToy, ...) publishes to the same FrameBus, so no per-device
// preview implementation is needed; the preview UI and worker thread are fixed.
//
// Implementations must be safe to call from a single dedicated worker thread
// (TryOpen / ReadFormat / WaitFrame / TryReadNewer / IsCameraInUse). They
// must not mutate any state visible to the camera capture thread.
//
#include <windows.h>
#include <cstdint>

class ICameraPreviewSource
{
public:
    virtual ~ICameraPreviewSource() = default;

    // Open the per-camera transport for reading. Idempotent while open.
    // Returns false if the producer is not up yet; the caller retries.
    virtual bool TryOpen(int cameraIndex) = 0;
    virtual bool IsOpen() const = 0;
    virtual void Close() = 0;

    // Current frame dimensions + frame rate. Implementations should return
    // false when the producer is not yet publishing a valid format.
    virtual bool ReadFormat(uint32_t& width, uint32_t& height, uint32_t& fps) = 0;

    // Block until a new frame *might* be available (or timeout). Purely a
    // wakeup hint; callers always re-check TryReadNewer afterwards.
    virtual void WaitFrame(DWORD timeoutMs) const = 0;

    // Copy the newest frame into dst iff it differs from lastFrameId and the
    // dimensions match dstBytes. Returns the consumed frameId, or 0 if there
    // is nothing new (or the source is not open / dims mismatch).
    virtual LONG64 TryReadNewer(uint8_t* dst, uint32_t dstBytes, LONG64 lastFrameId) = 0;

    // True when an external app (OBS, Discord, ...) is actively consuming
    // frames from this camera. Used purely to draw the "in use" badge on the
    // preview; never gates reading frames (the FrameBus is multi-reader).
    virtual bool IsCameraInUse(uint32_t idleTimeoutMs) const = 0;
};
