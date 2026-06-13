#pragma once
//
// FrameBus — single-producer / multi-reader shared-memory frame transport.
//
// The host app (PS3EyeVCamHost.exe, runs elevated in the user session) is the
// writer. The media source (PS3EyeVCam.dll, loaded by the Camera Frame Server
// service as LOCAL SERVICE in session 0) is the reader. A "Global\" section is
// the only namespace visible to both, which is why the host must be elevated
// (SeCreateGlobalPrivilege).
//
// Synchronization is a seqlock: the writer makes `seq` odd while copying and
// even when done; readers copy the payload and retry if `seq` changed. No
// mutexes cross the boundary, so a stuck reader can never stall capture and
// N readers (FrameServer + FrameServerMonitor) work for free.
//
// A named auto-reset event ("<name>.FrameReady") is pulsed once per publish so
// readers can sleep between frames instead of polling; it carries no data and
// is purely a wakeup hint — readers always re-validate against the seqlock.
//
#include <windows.h>
#include <sddl.h>
#include <atomic>
#include <cstdint>
#include <cstring>
#include "IpcNames.h"

namespace framebus {

constexpr uint32_t kMagic      = 0x50533345;  // 'PS3E'
constexpr uint32_t kVersion    = 2;
constexpr uint32_t kFormatYUY2 = 2;

// The payload is YUY2 (YUYV 4:2:2), the PS3 Eye's native output format. It
// preserves full vertical chroma resolution, so the DLL's YUY2 ("original
// format") clients receive the frame untouched; NV12 clients get a proper
// 4:2:2 -> 4:2:0 chroma downsample on the fly.
constexpr uint32_t kMaxWidth      = 640;      // PS3 Eye sensor maximum
constexpr uint32_t kMaxHeight     = 480;
constexpr uint32_t kDataOffset    = 4096;     // header page, then pixels
constexpr uint32_t kMaxFrameBytes = kMaxWidth * kMaxHeight * 2;  // YUY2
constexpr uint32_t kSectionBytes  = kDataOffset + kMaxFrameBytes;

#pragma pack(push, 8)
struct Header
{
    uint32_t magic;     // kMagic once the header below is fully valid
    uint32_t version;
    uint32_t width;
    uint32_t height;
    uint32_t fpsNum;
    uint32_t fpsDen;
    uint32_t format;    // kFormatYUY2
    uint32_t pad0;
    volatile LONG64 seq;      // seqlock: odd while the writer is copying
    volatile LONG64 frameId;  // increments once per published frame
    LONG64 qpc;               // QueryPerformanceCounter at capture time
};
#pragma pack(pop)

static_assert(sizeof(Header) <= kDataOffset, "header must fit in its page");

inline uint32_t Yuy2Bytes(uint32_t w, uint32_t h) { return w * h * 2; }
inline uint32_t Nv12Bytes(uint32_t w, uint32_t h) { return w * h * 3 / 2; }

// ---------------------------------------------------------------- Writer ----

class Writer
{
public:
    ~Writer() { Close(); }

    // Grants: SYSTEM/Admins/LOCAL SERVICE full, Everyone + app packages read.
    // LOCAL SERVICE is what the Frame Server runs as.
    bool Create(int cameraIndex, uint32_t width, uint32_t height, uint32_t fpsNum, uint32_t fpsDen)
    {
        wchar_t sectionName[64];
        wchar_t eventName[64];
        ipcnames::Format(sectionName, L".FrameBus", cameraIndex);
        ipcnames::Format(eventName, L".FrameReady", cameraIndex);

        SECURITY_ATTRIBUTES sa{ sizeof(sa), nullptr, FALSE };
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
                L"D:(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;LS)(A;;GRGWGX;;;WD)(A;;GRGWGX;;;AC)",
                SDDL_REVISION_1, &sa.lpSecurityDescriptor, nullptr))
            return false;

        _map = CreateFileMappingW(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE,
                                  0, kSectionBytes, sectionName);
        _lastError = GetLastError();
        LocalFree(sa.lpSecurityDescriptor);
        if (!_map)
            return false;

        // Frame-ready wakeup event. Readers only need open/wait rights, so
        // the world principals get GR|GX (GX maps to SYNCHRONIZE for events)
        // but NOT GW: an untrusted process must not be able to SetEvent.
        SECURITY_ATTRIBUTES evSa{ sizeof(evSa), nullptr, FALSE };
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
                L"D:(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;LS)(A;;GRGX;;;WD)(A;;GRGX;;;AC)",
                SDDL_REVISION_1, &evSa.lpSecurityDescriptor, nullptr))
        {
            Close();
            return false;
        }
        _frameReady = CreateEventW(&evSa, FALSE /*auto-reset*/, FALSE, eventName);
        LocalFree(evSa.lpSecurityDescriptor);
        if (!_frameReady) { _lastError = GetLastError(); Close(); return false; }

        _view = MapViewOfFile(_map, FILE_MAP_ALL_ACCESS, 0, 0, 0);
        if (!_view) { _lastError = GetLastError(); Close(); return false; }

        memset(_view, 0, kDataOffset);
        auto* h = Hdr();
        h->version = kVersion;
        h->width   = width;
        h->height  = height;
        h->fpsNum  = fpsNum;
        h->fpsDen  = fpsDen;
        h->format  = kFormatYUY2;
        h->seq     = 0;
        h->frameId = 0;
        MemoryBarrier();
        h->magic = kMagic;  // readers treat the section as live from here on
        return true;
    }

    void Publish(const uint8_t* yuy2, uint32_t bytes)
    {
        auto* h = Hdr();
        InterlockedIncrement64(&h->seq);            // odd: copy in progress
        memcpy(Data(), yuy2, bytes);
        LARGE_INTEGER qpc; QueryPerformanceCounter(&qpc);
        h->qpc = qpc.QuadPart;
        InterlockedIncrement64(&h->frameId);
        InterlockedIncrement64(&h->seq);            // even: frame consistent
        SignalFrameReady();
    }

    // Publishes a black frame in the current format. Used on camera-sleep
    // transitions: no stale image lingers in the (world-readable) section and
    // the next client's pre-wake frames are clean black.
    void PublishBlack()
    {
        auto* h = Hdr();
        InterlockedIncrement64(&h->seq);
        FillBlackPayload(h->width, h->height);
        LARGE_INTEGER qpc; QueryPerformanceCounter(&qpc);
        h->qpc = qpc.QuadPart;
        InterlockedIncrement64(&h->frameId);
        InterlockedIncrement64(&h->seq);
        SignalFrameReady();
    }

    // In-place format change under the seqlock. The section is sized for the
    // largest mode and is NEVER recreated: readers (the Frame Server) keep
    // their original mapping, and a name-reuse recreate would strand them on
    // an orphaned view. frameId stays monotonic — resetting it would make
    // readers treat every future frame as stale.
    void UpdateFormat(uint32_t width, uint32_t height, uint32_t fpsNum, uint32_t fpsDen)
    {
        auto* h = Hdr();
        InterlockedIncrement64(&h->seq);
        h->width  = width;
        h->height = height;
        h->fpsNum = fpsNum;
        h->fpsDen = fpsDen;
        FillBlackPayload(width, height);
        LARGE_INTEGER qpc; QueryPerformanceCounter(&qpc);
        h->qpc = qpc.QuadPart;
        InterlockedIncrement64(&h->frameId);
        InterlockedIncrement64(&h->seq);
        SignalFrameReady();
    }

    void Close()
    {
        if (_view)       { UnmapViewOfFile(_view); _view = nullptr; }
        if (_map)        { CloseHandle(_map); _map = nullptr; }
        if (_frameReady) { CloseHandle(_frameReady); _frameReady = nullptr; }
    }

    DWORD LastError() const { return _lastError; }

private:
    Header*  Hdr()  { return static_cast<Header*>(_view); }
    uint8_t* Data() { return static_cast<uint8_t*>(_view) + kDataOffset; }

    void SignalFrameReady()
    {
        if (_frameReady)
            SetEvent(_frameReady);
    }

    void FillBlackPayload(uint32_t w, uint32_t h)
    {
        // YUY2 black: Y 0x10, U/V 0x80, repeating byte pattern Y U Y V.
        uint32_t* data = reinterpret_cast<uint32_t*>(Data());
        const size_t words = static_cast<size_t>(w) * h / 2;  // 4 bytes per 2 px
        for (size_t i = 0; i < words; ++i)
            data[i] = 0x80108010u;
    }

    HANDLE _map = nullptr;
    HANDLE _frameReady = nullptr;
    void*  _view = nullptr;
    DWORD  _lastError = 0;
};

// ---------------------------------------------------------------- Reader ----

class Reader
{
public:
    ~Reader() { Close(); }

    bool TryOpen(int cameraIndex)
    {
        if (_view)
            return true;
        wchar_t sectionName[64];
        wchar_t eventName[64];
        ipcnames::Format(sectionName, L".FrameBus", cameraIndex);
        ipcnames::Format(eventName, L".FrameReady", cameraIndex);

        _map = OpenFileMappingW(FILE_MAP_READ, FALSE, sectionName);
        if (!_map)
            return false;
        _view = MapViewOfFile(_map, FILE_MAP_READ, 0, 0, 0);
        if (!_view) { Close(); return false; }
        if (Hdr()->magic != kMagic) { Close(); return false; }
        // The writer creates the event before publishing the magic, so a live
        // section implies the event exists. Treat a failed open as "not ready
        // yet" — the caller retries the whole open later.
        _frameReady = OpenEventW(SYNCHRONIZE, FALSE, eventName);
        if (!_frameReady) { Close(); return false; }
        return true;
    }

    bool IsOpen() const { return _view != nullptr; }

    void Close()
    {
        if (_view)       { UnmapViewOfFile(_view); _view = nullptr; }
        if (_map)        { CloseHandle(_map); _map = nullptr; }
        if (_frameReady) { CloseHandle(_frameReady); _frameReady = nullptr; }
    }

    bool ReadFormat(Header& out)
    {
        if (!_view)
            return false;
        out = *Hdr();
        return out.magic == kMagic && out.format == kFormatYUY2 &&
               out.width > 0 && out.width <= kMaxWidth &&
               out.height > 0 && out.height <= kMaxHeight &&
               out.fpsNum > 0 && out.fpsDen > 0;
    }

    // Sleeps until the writer pulses the frame-ready event (or the timeout
    // elapses). Purely a wakeup hint: callers re-check TryReadNewer after.
    void WaitFrame(DWORD timeoutMs) const
    {
        if (_frameReady)
            WaitForSingleObject(_frameReady, timeoutMs);
    }

    // Copies the newest frame into dst iff it differs from lastFrameId and the
    // dimensions match. Returns the consumed frameId, or 0 if nothing new.
    //
    // frameId moving BACKWARDS means the host restarted and re-created the
    // section (its counter restarts at 0 while the Frame Server kept the
    // mapping alive); the frame is accepted so the picture recovers instantly
    // instead of freezing until the counter catches back up.
    //
    // NOTE: this view is mapped FILE_MAP_READ, so the seqlock counters must be
    // read with plain aligned loads (atomic on x64). Interlocked* intrinsics
    // emit `lock` instructions that demand write access to the page and
    // access-violate on a read-only mapping.
    LONG64 TryReadNewer(uint8_t* dst, uint32_t dstBytes, LONG64 lastFrameId)
    {
        if (!_view)
            return 0;
        const Header* h = Hdr();
        if (h->magic != kMagic || Yuy2Bytes(h->width, h->height) != dstBytes)
            return 0;

        // Bounded spin: the writer's copy window is tens of microseconds; if
        // we keep colliding the caller waits on the frame-ready event and
        // retries, so giving up here never drops a frame permanently.
        for (int attempt = 0; attempt < 64; ++attempt)
        {
            const LONG64 seqBefore = ReadAcquire64(&h->seq);
            if (seqBefore & 1)
            {
                YieldProcessor();
                continue;
            }
            const LONG64 id = ReadAcquire64(&h->frameId);
            if (id == 0 || id == lastFrameId)
                return 0;

            memcpy(dst, Data(), dstBytes);

            // The fence orders the payload loads above before the seq
            // re-validation below (x64 keeps loads ordered anyway; this makes
            // the seqlock correct by construction, not by ISA accident).
            std::atomic_thread_fence(std::memory_order_acquire);
            const LONG64 seqAfter = ReadNoFence64(&h->seq);
            if (seqBefore == seqAfter)
                return id;  // copy was not torn
        }
        return 0;
    }

private:
    const Header*  Hdr()  const { return static_cast<const Header*>(_view); }
    const uint8_t* Data() const { return static_cast<const uint8_t*>(_view) + kDataOffset; }

    HANDLE _map = nullptr;
    HANDLE _frameReady = nullptr;
    void*  _view = nullptr;
};

} // namespace framebus
