#pragma once
//
// FrameBusPreviewSource — the FrameBus-backed implementation of
// ICameraPreviewSource. Transport-agnostic: it reads whatever the host is
// publishing (PS3 Eye, EyeToy, ...) since every device feeds the same FrameBus.
//
// Composes:
//   * framebus::Reader  (FILE_MAP_READ seqlock consumer; multi-reader safe,
//                        cannot affect the writer or any other reader such as
//                        the DLL serving OBS/Discord)
//   * controlbus::ActivityAge() (read-only query used for the "in use" badge)
//
// Lives entirely on the preview worker thread; no shared mutable state.
//
// INVARIANT (docs/TDD.md §5.3): this source MUST poll the seqlock (Sleep-based
// WaitFrame), never wait on the ".FrameReady" event. That event is auto-reset,
// so SetEvent releases exactly one waiter; the single permitted waiter is the
// DLL's DeliverSample. A preview that waited on it would steal wakeups and
// starve the virtual camera. Do not "optimize" WaitFrame into an event wait.
//
#include "ICameraPreviewSource.h"
#include "../common/FrameBus.h"

class FrameBusPreviewSource final : public ICameraPreviewSource
{
public:
    bool TryOpen(int cameraIndex) override;
    bool IsOpen() const override;
    void Close() override;
    bool ReadFormat(uint32_t& width, uint32_t& height, uint32_t& fps) override;
    void WaitFrame(DWORD timeoutMs) const override;
    LONG64 TryReadNewer(uint8_t* dst, uint32_t dstBytes, LONG64 lastFrameId) override;
    bool IsCameraInUse(uint32_t idleTimeoutMs) const override;

private:
    framebus::Reader _reader;
    int _cameraIndex = 0;
};
