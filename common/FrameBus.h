#pragma once
//
// FrameBus — single-producer / multi-reader shared-memory frame transport.
//
// The host app (PSCam4WinTray.exe, runs elevated in the user session) is the
// writer. The media source (PSCam4Win.dll, loaded by the Camera Frame Server
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
// NOTE: SetEvent on an auto-reset event releases exactly ONE waiter, so only a
// single reader per slot may WAIT on it (the Frame Server's DeliverSample);
// every other reader (preview, capability probe) must poll the seqlock. See
// docs/TDD.md §5.3.
//
// ---------------------------------------------------------------------------
// v3 layout (one section per slot, sized for the largest mode):
//   [   0 ..  63] HotHeader  — magic, version, payloadFormat, w, h, fps, jpegLen,
//                              seq, frameId, qpc   (everything TryReadNewer touches;
//                              one cache line, isolated from cold capability reads)
//   [  64 ..4095] ColdBlock  — transportClass, defaultFormat, modeCount, modeTable[]
//                              (queried once at wake; never on the per-frame path)
//   [4096 ..+YUY2] YUY2 pixels                 (kMaxFrameBytes)
//   [ then ..+JPEG] JPEG sidecar               (kMaxJpegBytes; filled only when an
//                              MJPEG client is active — PS3-only machines never touch it)
//
#include <windows.h>
#include <sddl.h>
#include <atomic>
#include <cstdint>
#include <cstring>
#include "IpcNames.h"

namespace framebus {

constexpr uint32_t kMagic      = 0x50533345;  // 'PS3E'
constexpr uint32_t kVersion    = 3;

// payloadFormat values. YUY2 is canonical: the host always publishes YUY2 (the
// PS3 Eye's native output, and the EyeToy's decoded output), so the no-poll
// fast path never branches on format. NV12 and beyond are reserved for future
// devices that may publish a different canonical buffer (docs/TDD.md §6.2).
constexpr uint32_t kFormatYUY2 = 2;
constexpr uint32_t kFormatNV12 = 3;

// Pixel-format bits for ColdBlock formatMask / defaultFormat. A set bit means
// the device can serve that format. Values match host FormatBits
// (ICameraDevice.h) and ControlBus ConsumerFormat so a bit means the same thing
// across capability, consumer-intent, and per-frame layers.
enum FormatMaskBits : uint32_t
{
    kFmtYUY2  = 1u,
    kFmtNV12  = 2u,
    kFmtMJPEG = 4u,
};

// The YUY2 payload (YUYV 4:2:2) preserves full vertical chroma resolution, so
// the DLL's YUY2 ("original format") clients receive the frame untouched; NV12
// clients get a proper 4:2:2 -> 4:2:0 chroma downsample on the fly.
constexpr uint32_t kMaxWidth      = 640;      // PS3 Eye sensor maximum
constexpr uint32_t kMaxHeight     = 480;
constexpr uint32_t kDataOffset    = 4096;     // header page (HotHeader + ColdBlock), then pixels
constexpr uint32_t kMaxFrameBytes = kMaxWidth * kMaxHeight * 2;  // YUY2
constexpr uint32_t kMaxJpegBytes  = 262144;   // 256 KiB JFIF sidecar (EyeToy MJPEG)
constexpr uint32_t kSectionBytes  = kDataOffset + kMaxFrameBytes + kMaxJpegBytes;

// ColdBlock lives at this offset, on its own cache line away from the seqlock
// fields so capability reads never false-share with per-frame publishes.
constexpr uint32_t kColdOffset    = 64;
constexpr uint32_t kMaxModes      = 32;       // >= PS3 Eye's 13 advertised modes

#pragma pack(push, 8)
struct HotHeader
{
    uint32_t magic;          // kMagic once the header below is fully valid
    uint32_t version;        // kVersion
    uint32_t payloadFormat;  // kFormatYUY2 (canonical) | kFormatNV12 (reserved)
    uint32_t width;
    uint32_t height;
    uint32_t fpsNum;
    uint32_t fpsDen;
    uint32_t jpegLen;        // JFIF byte count in the sidecar; 0 if none. <= kMaxJpegBytes.
    volatile LONG64 seq;     // seqlock: odd while the writer is copying
    volatile LONG64 frameId; // increments once per published frame
    LONG64 qpc;              // QueryPerformanceCounter at capture time
};

// One advertised capture mode (capability metadata, not per-frame).
struct ColdMode
{
    uint32_t width;
    uint32_t height;
    uint32_t fps;
    uint32_t formatMask;     // FMT_* bits the device may advertise for this mode
};

// Static capability block for the slot's device. Written once by the host
// before the virtual camera is registered (docs/TDD.md §8.3), then read by the
// DLL at activation to build its media-type list. transportClass/formatMask are
// stored as raw uint32_t so this header has no dependency on host/ICameraDevice.h.
struct ColdBlock
{
    uint32_t transportClass; // TransportClass of the slot's device
    uint32_t defaultFormat;  // FMT_* the DLL should default to (NV12 PS3, YUY2 EyeToy)
    uint32_t modeCount;      // valid entries in modes[]
    uint32_t pad0;
    ColdMode modes[kMaxModes];
};
#pragma pack(pop)

static_assert(sizeof(HotHeader) <= kColdOffset, "hot header must stay within one cache line");
static_assert(kColdOffset + sizeof(ColdBlock) <= kDataOffset, "cold block must fit in the header page");

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

        // Zero the whole header page (HotHeader + ColdBlock). The ColdBlock
        // stays modeCount==0 until WriteColdBlock; the host writes it before
        // registering the virtual camera, so the DLL never sees a live camera
        // without capabilities (docs/TDD.md §8.3).
        memset(_view, 0, kDataOffset);
        auto* h = Hdr();
        h->version       = kVersion;
        h->payloadFormat = kFormatYUY2;
        h->width   = width;
        h->height  = height;
        h->fpsNum  = fpsNum;
        h->fpsDen  = fpsDen;
        h->jpegLen = 0;
        h->seq     = 0;
        h->frameId = 0;
        MemoryBarrier();
        h->magic = kMagic;  // readers treat the section as live from here on
        return true;
    }

    // Writes the device capability block. Cold path: called once on the capture
    // thread before tryRegisterVCam(), so no reader can observe a torn block
    // (the DLL only opens the bus after the camera is registered). Stores are
    // followed by a release barrier so the modeCount/modes are visible before
    // any later magic-gated read on another core.
    void WriteColdBlock(uint32_t transportClass, uint32_t defaultFormat,
                        const ColdMode* modes, uint32_t modeCount)
    {
        if (!_view)
            return;
        if (modeCount > kMaxModes)
            modeCount = kMaxModes;
        auto* c = Cold();
        c->transportClass = transportClass;
        c->defaultFormat  = defaultFormat;
        c->pad0           = 0;
        for (uint32_t i = 0; i < modeCount; ++i)
            c->modes[i] = modes[i];
        MemoryBarrier();
        c->modeCount = modeCount;   // published last: nonzero means the rest is valid
    }

    void Publish(const uint8_t* yuy2, uint32_t bytes)
    {
        auto* h = Hdr();
        InterlockedIncrement64(&h->seq);            // odd: copy in progress
        memcpy(Data(), yuy2, bytes);
        h->jpegLen = 0;                             // no JFIF sidecar this frame
        LARGE_INTEGER qpc; QueryPerformanceCounter(&qpc);
        h->qpc = qpc.QuadPart;
        InterlockedIncrement64(&h->frameId);
        InterlockedIncrement64(&h->seq);            // even: frame consistent
        SignalFrameReady();
    }

    // Publishes a frame plus its source JFIF sidecar (EyeToy MJPEG passthrough
    // for native-MF MJPEG clients). Seqlock order per docs/TDD.md §5.4:
    //   seq++ -> YUY2 -> JPEG -> jpegLen -> qpc -> frameId++ -> seq++.
    // `len` is clamped to the sidecar capacity inside the window.
    void PublishWithJpeg(const uint8_t* yuy2, uint32_t bytes,
                         const uint8_t* jpeg, uint32_t len)
    {
        if (len > kMaxJpegBytes)
            len = kMaxJpegBytes;
        auto* h = Hdr();
        InterlockedIncrement64(&h->seq);
        memcpy(Data(), yuy2, bytes);
        if (jpeg && len)
            memcpy(Jpeg(), jpeg, len);
        h->jpegLen = (jpeg ? len : 0);
        LARGE_INTEGER qpc; QueryPerformanceCounter(&qpc);
        h->qpc = qpc.QuadPart;
        InterlockedIncrement64(&h->frameId);
        InterlockedIncrement64(&h->seq);
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
        h->jpegLen = 0;                             // drop any stale MJPEG sidecar
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
        h->jpegLen = 0;                             // drop any stale MJPEG sidecar
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
    HotHeader* Hdr()  { return static_cast<HotHeader*>(_view); }
    ColdBlock* Cold() { return reinterpret_cast<ColdBlock*>(static_cast<uint8_t*>(_view) + kColdOffset); }
    uint8_t*   Data() { return static_cast<uint8_t*>(_view) + kDataOffset; }
    uint8_t*   Jpeg() { return static_cast<uint8_t*>(_view) + kDataOffset + kMaxFrameBytes; }

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

    bool ReadFormat(HotHeader& out)
    {
        if (!_view)
            return false;
        out = *Hdr();
        return out.magic == kMagic && out.version == kVersion &&
               out.payloadFormat == kFormatYUY2 &&
               out.width > 0 && out.width <= kMaxWidth &&
               out.height > 0 && out.height <= kMaxHeight &&
               out.fpsNum > 0 && out.fpsDen > 0;
    }

    // Copies the device capability block. Cold path: the block is written once
    // before the camera is registered, so a plain copy (no seqlock) is correct.
    // Returns false until a valid, populated block is present.
    bool ReadColdBlock(ColdBlock& out)
    {
        if (!_view)
            return false;
        const HotHeader* h = Hdr();
        if (h->magic != kMagic || h->version != kVersion)
            return false;
        out = *Cold();
        return out.modeCount > 0 && out.modeCount <= kMaxModes;
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
        const HotHeader* h = Hdr();
        // version/magic are written once before the magic is published and never
        // change, so a single non-seqlocked check is sufficient. Rejecting a
        // version mismatch here stops an old reader from mis-decoding a section
        // re-created by a newer writer during the Frame Server restart window.
        if (h->magic != kMagic || h->version != kVersion ||
            Yuy2Bytes(h->width, h->height) != dstBytes)
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

    // MJPEG overload: copies the JFIF sidecar (and optionally the YUY2 frame)
    // atomically under the same seqlock window (docs/TDD.md §5.4). jpegLen is
    // read and clamped INSIDE the window so a torn/corrupt length can never
    // drive an over-read past the section or the caller's buffer. outJpegLen
    // receives the bytes actually copied (0 if the frame carries no sidecar).
    //
    // dstBytes always names the expected YUY2 size and is validated against the
    // current bus dimensions (the passthrough guard — a JPEG can't be rescaled
    // without decoding). Pass dst=nullptr for a JPEG-only client to skip the
    // YUY2 copy entirely. A YUY2-only caller uses the 3-arg overload above.
    LONG64 TryReadNewer(uint8_t* dst, uint32_t dstBytes,
                        uint8_t* jpegDst, uint32_t jpegCap, uint32_t& outJpegLen,
                        LONG64 lastFrameId)
    {
        outJpegLen = 0;
        if (!_view)
            return 0;
        const HotHeader* h = Hdr();
        if (h->magic != kMagic || h->version != kVersion ||
            Yuy2Bytes(h->width, h->height) != dstBytes)
            return 0;

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

            uint32_t len = h->jpegLen;          // read inside the window...
            if (len > kMaxJpegBytes) len = kMaxJpegBytes;  // ...then clamp before any copy
            if (jpegDst && len > jpegCap) len = jpegCap;

            if (dst)
                memcpy(dst, Data(), dstBytes);
            if (jpegDst && len)
                memcpy(jpegDst, Jpeg(), len);

            std::atomic_thread_fence(std::memory_order_acquire);
            const LONG64 seqAfter = ReadNoFence64(&h->seq);
            if (seqBefore == seqAfter)
            {
                outJpegLen = (jpegDst ? len : 0);
                return id;  // copy was not torn
            }
        }
        return 0;
    }

private:
    const HotHeader* Hdr()  const { return static_cast<const HotHeader*>(_view); }
    const ColdBlock* Cold() const { return reinterpret_cast<const ColdBlock*>(static_cast<const uint8_t*>(_view) + kColdOffset); }
    const uint8_t*   Data() const { return static_cast<const uint8_t*>(_view) + kDataOffset; }
    const uint8_t*   Jpeg() const { return static_cast<const uint8_t*>(_view) + kDataOffset + kMaxFrameBytes; }

    HANDLE _map = nullptr;
    HANDLE _frameReady = nullptr;
    void*  _view = nullptr;
};

} // namespace framebus
