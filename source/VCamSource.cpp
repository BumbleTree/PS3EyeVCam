//
// MediaSource / MediaStream implementation. See VCamSource.h for the overview.
//
#include "VCamSource.h"
#include <cstdarg>
#include <cstdio>

void VCamTrace(const wchar_t* fmt, ...)
{
    wchar_t buf[512];
    va_list args;
    va_start(args, fmt);
    _vsnwprintf_s(buf, _TRUNCATE, fmt, args);
    va_end(args);
    wchar_t line[560];
    _snwprintf_s(line, _TRUNCATE, L"PS3EyeVCam: %s\n", buf);
    OutputDebugStringW(line);
}

// ===========================================================================
// MediaStream
// ===========================================================================

MediaStream::MediaStream(MediaSource* parent, int cameraIndex) : _parent(parent), _cameraIndex(cameraIndex)
{
    DllAddRef();
}

namespace
{

// ---- SWAR helpers (x64 little-endian) -------------------------------------

inline uint64_t Load64(const uint8_t* p)
{
    uint64_t v;
    memcpy(&v, p, sizeof(v));
    return v;
}

inline void Store64(uint8_t* p, uint64_t v)
{
    memcpy(p, &v, sizeof(v));
}

// Rounding-up byte-wise average of 8 byte lanes: per byte, (a + b + 1) >> 1.
// Identity: avg_up(a,b) = (a | b) - (((a ^ b) >> 1) & 0x7f per lane).
inline uint64_t AvgBytes8(uint64_t a, uint64_t b)
{
    return (a | b) - (((a ^ b) >> 1) & 0x7f7f7f7f7f7f7f7full);
}

// Bilinear 2x upscale in the YUY2 domain (e.g. 320x240 -> 640x480).
//
// Pass 1 writes every even output row: luma and chroma are doubled with the
// in-between samples linearly interpolated from their horizontal neighbours
// (edge samples replicate). Pass 2 fills the odd rows as the byte-wise mean
// of the two adjacent even rows — in YUY2 every byte column holds the same
// component on every row, so a byte-wise average IS a true vertical lerp.
void UpscaleYuy2_2x(const uint8_t* src, uint8_t* dst, uint32_t srcW, uint32_t srcH)
{
    const uint32_t srcStride = srcW * 2;
    const uint32_t dstStride = srcW * 4;        // (srcW * 2 px) * 2 bytes
    const uint32_t srcPairs  = srcW / 2;        // YUYV macropixels per row

    for (uint32_t y = 0; y < srcH; ++y)
    {
        const uint8_t* s = src + y * srcStride;
        uint8_t* d = dst + (y * 2) * dstStride;

        for (uint32_t i = 0; i < srcPairs; ++i)
        {
            const uint32_t y0 = s[i * 4 + 0], u0 = s[i * 4 + 1];
            const uint32_t y1 = s[i * 4 + 2], v0 = s[i * 4 + 3];
            const bool last = (i + 1 == srcPairs);
            const uint32_t y2 = last ? y1 : s[i * 4 + 4];
            const uint32_t u1 = last ? u0 : s[i * 4 + 5];
            const uint32_t v1 = last ? v0 : s[i * 4 + 7];

            uint8_t* o = d + i * 8;
            o[0] = static_cast<uint8_t>(y0);
            o[1] = static_cast<uint8_t>(u0);
            o[2] = static_cast<uint8_t>((y0 + y1 + 1) >> 1);
            o[3] = static_cast<uint8_t>(v0);
            o[4] = static_cast<uint8_t>(y1);
            o[5] = static_cast<uint8_t>((u0 + u1 + 1) >> 1);
            o[6] = static_cast<uint8_t>((y1 + y2 + 1) >> 1);
            o[7] = static_cast<uint8_t>((v0 + v1 + 1) >> 1);
        }
    }

    // In YUY2 every byte column holds the same component on every row, so a
    // byte-wise average IS a true vertical lerp; do it 8 bytes per op.
    for (uint32_t y = 0; y + 1 < srcH; ++y)
    {
        const uint8_t* a = dst + (y * 2) * dstStride;
        const uint8_t* b = a + 2 * dstStride;
        uint8_t* o = dst + (y * 2 + 1) * dstStride;
        uint32_t x = 0;
        for (; x + 8 <= dstStride; x += 8)
            Store64(o + x, AvgBytes8(Load64(a + x), Load64(b + x)));
        for (; x < dstStride; ++x)
            o[x] = static_cast<uint8_t>((a[x] + b[x] + 1) >> 1);
    }
    memcpy(dst + (srcH * 2 - 1) * dstStride, dst + (srcH * 2 - 2) * dstStride, dstStride);
}

// 2x2 box-filter downscale in the YUY2 domain (e.g. 640x480 -> 320x240).
// Every output sample is the mean of the four source samples it covers,
// which avoids the aliasing of point sampling.
void DownscaleYuy2_2x(const uint8_t* src, uint8_t* dst, uint32_t srcW, uint32_t srcH)
{
    const uint32_t srcStride = srcW * 2;
    const uint32_t dstStride = srcW;            // (srcW / 2 px) * 2 bytes
    const uint32_t dstPairs  = srcW / 4;        // output macropixels per row

    for (uint32_t y = 0; y < srcH / 2; ++y)
    {
        const uint8_t* r0 = src + (y * 2) * srcStride;
        const uint8_t* r1 = r0 + srcStride;
        uint8_t* d = dst + y * dstStride;

        for (uint32_t i = 0; i < dstPairs; ++i)
        {
            const uint8_t* s0 = r0 + i * 8;     // two source macropixels...
            const uint8_t* s1 = r1 + i * 8;     // ...on each of two rows
            uint8_t* o = d + i * 4;
            o[0] = static_cast<uint8_t>((s0[0] + s0[2] + s1[0] + s1[2] + 2) >> 2);  // Y
            o[1] = static_cast<uint8_t>((s0[1] + s0[5] + s1[1] + s1[5] + 2) >> 2);  // U
            o[2] = static_cast<uint8_t>((s0[4] + s0[6] + s1[4] + s1[6] + 2) >> 2);  // Y
            o[3] = static_cast<uint8_t>((s0[3] + s0[7] + s1[3] + s1[7] + 2) >> 2);  // V
        }
    }
}

void FillYuy2Black(uint8_t* dst, uint32_t w, uint32_t h)
{
    // YUY2 black: Y 0x10, U/V 0x80, repeating byte pattern Y U Y V.
    uint32_t* p = reinterpret_cast<uint32_t*>(dst);
    const size_t words = static_cast<size_t>(w) * h / 2;  // 4 bytes per 2 px
    for (size_t i = 0; i < words; ++i)
        p[i] = 0x80108010u;
}

// YUY2 (4:2:2) -> NV12 (4:2:0). Luma is copied; each output chroma sample is
// the mean of the two source rows it replaces — the correct 4:2:0 downsample.
//
// Inner loop works on 8 source bytes per row (4 pixels / 2 macropixels): one
// SWAR average yields both rows' chroma means at once (lumas in the averaged
// word are simply ignored), and the lumas are picked out of the loaded words.
void Yuy2ToNv12(const uint8_t* src, uint8_t* dst, uint32_t w, uint32_t h)
{
    const uint32_t srcStride = w * 2;
    uint8_t* dstY  = dst;
    uint8_t* dstUV = dst + static_cast<size_t>(w) * h;

    for (uint32_t y = 0; y < h; y += 2)
    {
        const uint8_t* p0 = src + y * srcStride;
        const uint8_t* p1 = p0 + srcStride;
        uint8_t* y0 = dstY + y * w;
        uint8_t* y1 = y0 + w;
        uint8_t* uv = dstUV + (y / 2) * w;

        uint32_t x = 0;
        for (; x + 4 <= w; x += 4)
        {
            const uint64_t a   = Load64(p0);   // Y0 U0 Y1 V0 Y2 U1 Y3 V1
            const uint64_t b   = Load64(p1);
            const uint64_t avg = AvgBytes8(a, b);

            y0[0] = static_cast<uint8_t>(a);
            y0[1] = static_cast<uint8_t>(a >> 16);
            y0[2] = static_cast<uint8_t>(a >> 32);
            y0[3] = static_cast<uint8_t>(a >> 48);
            y1[0] = static_cast<uint8_t>(b);
            y1[1] = static_cast<uint8_t>(b >> 16);
            y1[2] = static_cast<uint8_t>(b >> 32);
            y1[3] = static_cast<uint8_t>(b >> 48);
            uv[0] = static_cast<uint8_t>(avg >> 8);   // U
            uv[1] = static_cast<uint8_t>(avg >> 24);  // V
            uv[2] = static_cast<uint8_t>(avg >> 40);  // U
            uv[3] = static_cast<uint8_t>(avg >> 56);  // V

            p0 += 8; p1 += 8; y0 += 4; y1 += 4; uv += 4;
        }
        for (; x < w; x += 2)
        {
            y0[0] = p0[0];
            y0[1] = p0[2];
            y1[0] = p1[0];
            y1[1] = p1[2];
            uv[0] = static_cast<uint8_t>((p0[1] + p1[1] + 1) >> 1);
            uv[1] = static_cast<uint8_t>((p0[3] + p1[3] + 1) >> 1);
            p0 += 4; p1 += 4; y0 += 2; y1 += 2; uv += 2;
        }
    }
}

} // namespace

HRESULT MediaStream::Initialize()
{
    // The host publishes the FrameBus before registering the virtual camera,
    // so normally the real capture format is already known here. The fallback
    // only triggers if the source is activated while the host is not running.
    framebus::Header fmt{};
    uint32_t defaultWidth = 640, defaultHeight = 480;
    uint32_t defaultFpsNum = 60, defaultFpsDen = 1;
    if (_bus.TryOpen(_cameraIndex) && _bus.ReadFormat(fmt))
    {
        defaultWidth = fmt.width;
        defaultHeight = fmt.height;
        defaultFpsNum = fmt.fpsNum;
        defaultFpsDen = fmt.fpsDen;
        VCamTrace(L"stream: FrameBus default format %ux%u @ %u/%u", defaultWidth, defaultHeight, defaultFpsNum, defaultFpsDen);
    }
    else
    {
        VCamTrace(L"stream: FrameBus not available, defaulting to 640x480@60");
    }

    _width = defaultWidth;
    _height = defaultHeight;
    _fpsNum = defaultFpsNum;
    _fpsDen = defaultFpsDen;
    _subtype = MFVideoFormat_NV12;
    _frameBytes = framebus::Nv12Bytes(_width, _height);
    _frameDuration = MulDiv(10000000, _fpsDen, _fpsNum);

    // _staging always holds the frame in YUY2 (the bus/native format) at the
    // negotiated size; NV12 clients are converted at delivery time.
    _staging.reset(new (std::nothrow) uint8_t[framebus::Yuy2Bytes(_width, _height)]);
    if (!_staging)
        return E_OUTOFMEMORY;
    FillBlack();

    _busStaging.reset(new (std::nothrow) uint8_t[framebus::kMaxFrameBytes]);
    if (!_busStaging)
        return E_OUTOFMEMORY;

    HRESULT hr = MFCreateEventQueue(&_queue);
    if (FAILED(hr)) return hr;

    hr = MFCreateAttributes(&_attributes, 4);
    if (FAILED(hr)) return hr;
    _attributes->SetGUID(MF_DEVICESTREAM_STREAM_CATEGORY, kPinCategoryCapture);
    _attributes->SetUINT32(MF_DEVICESTREAM_STREAM_ID, 0);
    _attributes->SetUINT32(MF_DEVICESTREAM_FRAMESERVER_SHARED, 1);
    _attributes->SetUINT32(MF_DEVICESTREAM_ATTRIBUTE_FRAMESOURCE_TYPES, kFrameSourceTypeColor);

    std::vector<IMFMediaType*> typesList;
    ComPtr<IMFMediaType> defaultType;

    for (int i = 0; i < kVideoModeCount; ++i)
    {
        // 1. Add NV12 (default)
        ComPtr<IMFMediaType> mtNV12;
        hr = CreateMediaType(kVideoModes[i].width, kVideoModes[i].height, kVideoModes[i].fps, 1, MFVideoFormat_NV12, &mtNV12);
        if (SUCCEEDED(hr))
        {
            typesList.push_back(mtNV12.Get());
            mtNV12->AddRef();

            if (kVideoModes[i].width == defaultWidth &&
                kVideoModes[i].height == defaultHeight &&
                kVideoModes[i].fps == defaultFpsNum)
            {
                defaultType = mtNV12;
            }
        }

        // 2. Add YUY2
        ComPtr<IMFMediaType> mtYUY2;
        hr = CreateMediaType(kVideoModes[i].width, kVideoModes[i].height, kVideoModes[i].fps, 1, MFVideoFormat_YUY2, &mtYUY2);
        if (SUCCEEDED(hr))
        {
            typesList.push_back(mtYUY2.Get());
            mtYUY2->AddRef();
        }
    }

    if (typesList.empty())
        return E_FAIL;

    if (!defaultType)
    {
        defaultType = typesList[0];
    }

    hr = MFCreateStreamDescriptor(0, static_cast<DWORD>(typesList.size()), typesList.data(), &_descriptor);

    for (auto* mt : typesList)
    {
        mt->Release();
    }

    if (FAILED(hr)) return hr;

    ComPtr<IMFMediaTypeHandler> handler;
    hr = _descriptor->GetMediaTypeHandler(&handler);
    if (FAILED(hr)) return hr;
    hr = handler->SetCurrentMediaType(defaultType.Get());
    if (FAILED(hr)) return hr;

    _parentRef = static_cast<IMFMediaSourceEx*>(_parent);
    return S_OK;
}

HRESULT MediaStream::CreateMediaType(uint32_t width, uint32_t height, uint32_t fpsNum, uint32_t fpsDen, GUID subtype, IMFMediaType** ppType)
{
    ComPtr<IMFMediaType> mt;
    HRESULT hr = MFCreateMediaType(&mt);
    if (FAILED(hr)) return hr;

    mt->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    mt->SetGUID(MF_MT_SUBTYPE, subtype);
    MFSetAttributeSize(mt.Get(), MF_MT_FRAME_SIZE, width, height);
    MFSetAttributeRatio(mt.Get(), MF_MT_FRAME_RATE, fpsNum, fpsDen);
    MFSetAttributeRatio(mt.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    mt->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    mt->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
    mt->SetUINT32(MF_MT_FIXED_SIZE_SAMPLES, TRUE);

    uint32_t sampleSize = 0;
    if (subtype == MFVideoFormat_NV12)
    {
        sampleSize = framebus::Nv12Bytes(width, height);
        mt->SetUINT32(MF_MT_DEFAULT_STRIDE, width);
    }
    else if (subtype == MFVideoFormat_YUY2)
    {
        sampleSize = width * height * 2;
        mt->SetUINT32(MF_MT_DEFAULT_STRIDE, width * 2);
    }
    mt->SetUINT32(MF_MT_SAMPLE_SIZE, sampleSize);

    *ppType = mt.Detach();
    return S_OK;
}

void MediaStream::FillBlack()
{
    FillYuy2Black(_staging.get(), _width, _height);
}

// Stamp the keepalive (and optionally pulse the wake event) so the host knows
// a client is consuming frames. Caller holds _lock. No-ops while the host's
// control objects don't exist; retries opening at most every 500ms.
void MediaStream::PingActivity(bool forceWakeSignal)
{
    const ULONGLONG now = GetTickCount64();
    if (!_ping.IsOpen())
    {
        if (now < _nextPingRetryTick)
            return;
        _nextPingRetryTick = now + 500;
        if (!_ping.TryOpen(_cameraIndex))
            return;
    }
    _ping.Stamp();                          // always tick BEFORE event
    if (forceWakeSignal || now - _lastWakeSignalTick >= 1000)
    {
        _lastWakeSignalTick = now;
        _ping.SignalWake();
    }
}

HRESULT MediaStream::Start(const PROPVARIANT* startPosition)
{
    AutoLock lock(_lock);
    if (_shutdown)
        return MF_E_SHUTDOWN;

    // Stage everything fallible BEFORE committing any live state: a stream
    // marked RUNNING with a null staging buffer would fault inside the Frame
    // Server on the next RequestSample.
    uint32_t width = _width, height = _height, fpsNum = _fpsNum, fpsDen = _fpsDen;
    GUID subtype = _subtype;

    ComPtr<IMFMediaTypeHandler> handler;
    ComPtr<IMFMediaType> currentType;
    if (SUCCEEDED(_descriptor->GetMediaTypeHandler(&handler)) &&
        SUCCEEDED(handler->GetCurrentMediaType(&currentType)))
    {
        MFGetAttributeSize(currentType.Get(), MF_MT_FRAME_SIZE, &width, &height);
        MFGetAttributeRatio(currentType.Get(), MF_MT_FRAME_RATE, &fpsNum, &fpsDen);
        subtype = MFVideoFormat_NV12;
        currentType->GetGUID(MF_MT_SUBTYPE, &subtype);
    }

    std::shared_ptr<uint8_t[]> staging(new (std::nothrow) uint8_t[framebus::Yuy2Bytes(width, height)]);
    if (!staging)
        return E_OUTOFMEMORY;
    FillYuy2Black(staging.get(), width, height);

    _width = width;
    _height = height;
    _fpsNum = fpsNum;
    _fpsDen = fpsDen;
    _subtype = subtype;
    _frameBytes = (subtype == MFVideoFormat_YUY2) ? framebus::Yuy2Bytes(width, height)
                                                  : framebus::Nv12Bytes(width, height);
    _frameDuration = MulDiv(10000000, fpsDen, fpsNum);
    _staging = std::move(staging);
    _state = MF_STREAM_STATE_RUNNING;
    _lastFrameId = 0;  // re-sync with whatever the bus currently holds

    VCamTrace(L"stream: started with format %ux%u @ %u/%u (subtype YUY2=%d)",
              width, height, fpsNum, fpsDen, (subtype == MFVideoFormat_YUY2));
    PingActivity(true);
    return _queue->QueueEventParamVar(MEStreamStarted, GUID_NULL, S_OK, startPosition);
}

HRESULT MediaStream::Stop()
{
    AutoLock lock(_lock);
    if (_shutdown)
        return MF_E_SHUTDOWN;
    _state = MF_STREAM_STATE_STOPPED;
    VCamTrace(L"stream: stopped");
    return _queue->QueueEventParamVar(MEStreamStopped, GUID_NULL, S_OK, nullptr);
}

HRESULT MediaStream::Shutdown()
{
    AutoLock lock(_lock);
    if (_shutdown)
        return S_OK;
    _shutdown = true;
    _state = MF_STREAM_STATE_STOPPED;
    if (_queue)
        _queue->Shutdown();
    // _bus is intentionally NOT closed here: a DeliverSample call on another
    // thread may still be polling the mapped view. The destructor closes it
    // once no method can be executing.
    _descriptor.Reset();
    _attributes.Reset();
    _parentRef.Reset();   // break the source<->stream reference cycle
    _parent = nullptr;
    VCamTrace(L"stream: shutdown");
    return S_OK;
}

// ----- IUnknown -------------------------------------------------------------

STDMETHODIMP MediaStream::QueryInterface(REFIID riid, void** ppv)
{
    if (!ppv)
        return E_POINTER;
    *ppv = nullptr;
    if (riid == IID_IUnknown || riid == __uuidof(IMFMediaStream2) ||
        riid == __uuidof(IMFMediaStream) || riid == __uuidof(IMFMediaEventGenerator))
        *ppv = static_cast<IMFMediaStream2*>(this);
    else if (riid == __uuidof(IKsControl))
        *ppv = static_cast<IKsControl*>(this);
    else
        return E_NOINTERFACE;
    AddRef();
    return S_OK;
}

STDMETHODIMP_(ULONG) MediaStream::AddRef()
{
    return InterlockedIncrement(&_refCount);
}

STDMETHODIMP_(ULONG) MediaStream::Release()
{
    const ULONG ref = InterlockedDecrement(&_refCount);
    if (ref == 0)
    {
        delete this;
        DllRelease();
    }
    return ref;
}

// ----- IMFMediaEventGenerator ------------------------------------------------

STDMETHODIMP MediaStream::BeginGetEvent(IMFAsyncCallback* pCallback, IUnknown* punkState)
{
    AutoLock lock(_lock);
    if (_shutdown) return MF_E_SHUTDOWN;
    return _queue->BeginGetEvent(pCallback, punkState);
}

STDMETHODIMP MediaStream::EndGetEvent(IMFAsyncResult* pResult, IMFMediaEvent** ppEvent)
{
    AutoLock lock(_lock);
    if (_shutdown) return MF_E_SHUTDOWN;
    return _queue->EndGetEvent(pResult, ppEvent);
}

STDMETHODIMP MediaStream::GetEvent(DWORD dwFlags, IMFMediaEvent** ppEvent)
{
    // Never hold the object lock across the (potentially blocking) GetEvent.
    ComPtr<IMFMediaEventQueue> queue;
    {
        AutoLock lock(_lock);
        if (_shutdown) return MF_E_SHUTDOWN;
        queue = _queue;
    }
    return queue->GetEvent(dwFlags, ppEvent);
}

STDMETHODIMP MediaStream::QueueEvent(MediaEventType met, REFGUID guidExtendedType,
                                     HRESULT hrStatus, const PROPVARIANT* pvValue)
{
    AutoLock lock(_lock);
    if (_shutdown) return MF_E_SHUTDOWN;
    return _queue->QueueEventParamVar(met, guidExtendedType, hrStatus, pvValue);
}

// ----- IMFMediaStream ---------------------------------------------------------

STDMETHODIMP MediaStream::GetMediaSource(IMFMediaSource** ppMediaSource)
{
    if (!ppMediaSource)
        return E_POINTER;
    AutoLock lock(_lock);
    if (_shutdown || !_parentRef)
        return MF_E_SHUTDOWN;
    *ppMediaSource = _parentRef.Get();
    (*ppMediaSource)->AddRef();
    return S_OK;
}

STDMETHODIMP MediaStream::GetStreamDescriptor(IMFStreamDescriptor** ppStreamDescriptor)
{
    if (!ppStreamDescriptor)
        return E_POINTER;
    AutoLock lock(_lock);
    if (_shutdown)
        return MF_E_SHUTDOWN;
    *ppStreamDescriptor = _descriptor.Get();
    (*ppStreamDescriptor)->AddRef();
    return S_OK;
}

STDMETHODIMP MediaStream::RequestSample(IUnknown* pToken)
{
    {
        AutoLock lock(_lock);
        if (_shutdown)
            return MF_E_SHUTDOWN;
        if (_state != MF_STREAM_STATE_RUNNING)
            return MF_E_MEDIA_SOURCE_WRONGSTATE;
        PingActivity(false);
    }
    return DeliverSample(pToken);
}

HRESULT MediaStream::DeliverSample(IUnknown* token)
{
    // Serialize deliveries: the staging buffers below belong to exactly one
    // in-flight DeliverSample at a time. _deliverLock is never taken by any
    // other method, so Start/Stop/Shutdown can't block on a delivery.
    AutoLock deliverLock(_deliverLock);

    // Snapshot the negotiated format and the staging buffer under the object
    // lock. Start() may renegotiate (swap _staging, rewrite the dimensions)
    // concurrently; working from a consistent snapshot prevents both torn
    // reads and a use-after-free of the buffer (the shared_ptr keeps the old
    // buffer alive until this call finishes with it).
    GUID     subtype;
    uint32_t width, height, frameBytes;
    LONGLONG frameDuration;
    std::shared_ptr<uint8_t[]> staging;
    {
        AutoLock lock(_lock);
        if (_shutdown)
            return MF_E_SHUTDOWN;
        if (_state != MF_STREAM_STATE_RUNNING)
            return MF_E_MEDIA_SOURCE_WRONGSTATE;
        subtype       = _subtype;
        width         = _width;
        height        = _height;
        frameBytes    = _frameBytes;
        frameDuration = _frameDuration;
        staging       = _staging;
    }

    // Pace delivery to the camera: wait for a frame newer than the last one
    // we handed out, sleeping on the writer's frame-ready event between
    // attempts (Sleep-based polling was quantized to the 15.6ms system timer
    // and silently capped the 100..187fps modes). On deadline we re-deliver
    // the previous frame so the pipeline keeps flowing even if the host
    // stalls or exits.
    const DWORD periodMs = static_cast<DWORD>(frameDuration / 10000);
    const ULONGLONG deadline = GetTickCount64() + periodMs * 2 + 50;

    for (;;)
    {
        {
            AutoLock lock(_lock);
            if (_shutdown)
                return MF_E_SHUTDOWN;
            if (_state != MF_STREAM_STATE_RUNNING)
                return MF_E_MEDIA_SOURCE_WRONGSTATE;
        }
        if (!_bus.IsOpen())
        {
            const ULONGLONG now = GetTickCount64();
            if (now >= _nextBusRetryTick)
            {
                _nextBusRetryTick = now + 500;
                if (_bus.TryOpen(_cameraIndex))
                    continue;
            }
        }
        else
        {
            framebus::Header fmt{};
            if (_bus.ReadFormat(fmt))
            {
                const uint32_t busWidth = fmt.width;
                const uint32_t busHeight = fmt.height;
                const uint32_t busFrameBytes = framebus::Yuy2Bytes(busWidth, busHeight);
                const LONG64 lastFrameId = _lastFrameId.load(std::memory_order_relaxed);

                if (busWidth == width && busHeight == height)
                {
                    const LONG64 id = _bus.TryReadNewer(staging.get(), busFrameBytes, lastFrameId);
                    if (id != 0)
                    {
                        _lastFrameId.store(id, std::memory_order_relaxed);
                        break;
                    }
                }
                else
                {
                    const LONG64 id = _bus.TryReadNewer(_busStaging.get(), busFrameBytes, lastFrameId);
                    if (id != 0)
                    {
                        _lastFrameId.store(id, std::memory_order_relaxed);
                        if (busWidth == 320 && busHeight == 240 && width == 640 && height == 480)
                        {
                            UpscaleYuy2_2x(_busStaging.get(), staging.get(), 320, 240);
                        }
                        else if (busWidth == 640 && busHeight == 480 && width == 320 && height == 240)
                        {
                            DownscaleYuy2_2x(_busStaging.get(), staging.get(), 640, 480);
                        }
                        else
                        {
                            FillYuy2Black(staging.get(), width, height);
                        }
                        break;
                    }
                }
            }
        }
        const ULONGLONG now = GetTickCount64();
        if (now >= deadline)
            break;
        const ULONGLONG remaining = deadline - now;
        const DWORD waitMs = remaining > 100 ? 100 : static_cast<DWORD>(remaining);
        if (_bus.IsOpen())
            _bus.WaitFrame(waitMs);          // woken by the writer per publish
        else
            Sleep(waitMs > 20 ? 20 : waitMs);  // host absent: cheap idle wait
    }

    ComPtr<IMFSample> sample;
    ComPtr<IMFMediaBuffer> buffer;
    HRESULT hr = MFCreateSample(&sample);
    if (SUCCEEDED(hr)) hr = MFCreateMemoryBuffer(frameBytes, &buffer);
    if (SUCCEEDED(hr))
    {
        BYTE* data = nullptr;
        DWORD maxLen = 0;
        hr = buffer->Lock(&data, &maxLen, nullptr);
        if (SUCCEEDED(hr))
        {
            if (subtype == MFVideoFormat_YUY2)
            {
                // Native path: the bus frame is already YUY2 — no conversion.
                memcpy(data, staging.get(), frameBytes);
            }
            else
            {
                Yuy2ToNv12(staging.get(), data, width, height);
            }
            buffer->Unlock();
            buffer->SetCurrentLength(frameBytes);
        }
    }
    if (SUCCEEDED(hr)) hr = sample->AddBuffer(buffer.Get());
    if (SUCCEEDED(hr)) hr = sample->SetSampleTime(MFGetSystemTime());
    if (SUCCEEDED(hr)) hr = sample->SetSampleDuration(frameDuration);
    if (SUCCEEDED(hr) && token)
        hr = sample->SetUnknown(MFSampleExtension_Token, token);

    if (SUCCEEDED(hr))
    {
        AutoLock lock(_lock);
        if (_shutdown)
            return MF_E_SHUTDOWN;
        if (_state != MF_STREAM_STATE_RUNNING)
            return MF_E_MEDIA_SOURCE_WRONGSTATE;
        hr = _queue->QueueEventParamUnk(MEMediaSample, GUID_NULL, S_OK, sample.Get());
    }
    return hr;
}

// ----- IMFMediaStream2 --------------------------------------------------------

STDMETHODIMP MediaStream::SetStreamState(MF_STREAM_STATE value)
{
    switch (value)
    {
    case MF_STREAM_STATE_RUNNING:
    {
        PROPVARIANT empty;
        PropVariantInit(&empty);
        return Start(&empty);
    }
    case MF_STREAM_STATE_STOPPED:
        return Stop();
    default:
        return MF_E_INVALID_STATE_TRANSITION;
    }
}

STDMETHODIMP MediaStream::GetStreamState(MF_STREAM_STATE* value)
{
    if (!value)
        return E_POINTER;
    AutoLock lock(_lock);
    if (_shutdown)
        return MF_E_SHUTDOWN;
    *value = _state;
    return S_OK;
}

// ----- IKsControl --------------------------------------------------------------

STDMETHODIMP MediaStream::KsProperty(void*, ULONG, void*, ULONG, ULONG* BytesReturned)
{
    if (BytesReturned) *BytesReturned = 0;
    return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}
STDMETHODIMP MediaStream::KsMethod(void*, ULONG, void*, ULONG, ULONG* BytesReturned)
{
    if (BytesReturned) *BytesReturned = 0;
    return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}
STDMETHODIMP MediaStream::KsEvent(void*, ULONG, void*, ULONG, ULONG* BytesReturned)
{
    if (BytesReturned) *BytesReturned = 0;
    return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}

// ===========================================================================
// MediaSource
// ===========================================================================

MediaSource::MediaSource(int cameraIndex) : _cameraIndex(cameraIndex)
{
    DllAddRef();
}

HRESULT MediaSource::Initialize(IMFAttributes* activationAttributes)
{
    HRESULT hr = MFCreateEventQueue(&_queue);
    if (FAILED(hr)) return hr;

    hr = MFCreateAttributes(&_attributes, 2);
    if (FAILED(hr)) return hr;
    if (activationAttributes)
        activationAttributes->CopyAllItems(_attributes.Get());

    _stream.Attach(new (std::nothrow) MediaStream(this, _cameraIndex));
    if (!_stream)
        return E_OUTOFMEMORY;
    hr = _stream->Initialize();
    if (FAILED(hr)) return hr;

    IMFStreamDescriptor* descriptors[] = { _stream->Descriptor() };
    hr = MFCreatePresentationDescriptor(1, descriptors, &_pd);
    if (FAILED(hr)) return hr;
    hr = _pd->SelectStream(0);
    if (FAILED(hr)) return hr;

    VCamTrace(L"source: initialized");
    return S_OK;
}

// ----- IUnknown -------------------------------------------------------------

STDMETHODIMP MediaSource::QueryInterface(REFIID riid, void** ppv)
{
    if (!ppv)
        return E_POINTER;
    *ppv = nullptr;
    if (riid == IID_IUnknown || riid == __uuidof(IMFMediaSourceEx) ||
        riid == __uuidof(IMFMediaSource) || riid == __uuidof(IMFMediaEventGenerator))
        *ppv = static_cast<IMFMediaSourceEx*>(this);
    else if (riid == __uuidof(IMFGetService))
        *ppv = static_cast<IMFGetService*>(this);
    else if (riid == __uuidof(IKsControl))
        *ppv = static_cast<IKsControl*>(this);
    else
        return E_NOINTERFACE;
    AddRef();
    return S_OK;
}

STDMETHODIMP_(ULONG) MediaSource::AddRef()
{
    return InterlockedIncrement(&_refCount);
}

STDMETHODIMP_(ULONG) MediaSource::Release()
{
    const ULONG ref = InterlockedDecrement(&_refCount);
    if (ref == 0)
    {
        delete this;
        DllRelease();
    }
    return ref;
}

// ----- IMFMediaEventGenerator ------------------------------------------------

STDMETHODIMP MediaSource::BeginGetEvent(IMFAsyncCallback* pCallback, IUnknown* punkState)
{
    AutoLock lock(_lock);
    HRESULT hr = CheckShutdown();
    if (FAILED(hr)) return hr;
    return _queue->BeginGetEvent(pCallback, punkState);
}

STDMETHODIMP MediaSource::EndGetEvent(IMFAsyncResult* pResult, IMFMediaEvent** ppEvent)
{
    AutoLock lock(_lock);
    HRESULT hr = CheckShutdown();
    if (FAILED(hr)) return hr;
    return _queue->EndGetEvent(pResult, ppEvent);
}

STDMETHODIMP MediaSource::GetEvent(DWORD dwFlags, IMFMediaEvent** ppEvent)
{
    ComPtr<IMFMediaEventQueue> queue;
    {
        AutoLock lock(_lock);
        HRESULT hr = CheckShutdown();
        if (FAILED(hr)) return hr;
        queue = _queue;
    }
    return queue->GetEvent(dwFlags, ppEvent);
}

STDMETHODIMP MediaSource::QueueEvent(MediaEventType met, REFGUID guidExtendedType,
                                     HRESULT hrStatus, const PROPVARIANT* pvValue)
{
    AutoLock lock(_lock);
    HRESULT hr = CheckShutdown();
    if (FAILED(hr)) return hr;
    return _queue->QueueEventParamVar(met, guidExtendedType, hrStatus, pvValue);
}

// ----- IMFMediaSource ---------------------------------------------------------

STDMETHODIMP MediaSource::CreatePresentationDescriptor(IMFPresentationDescriptor** ppPD)
{
    if (!ppPD)
        return E_POINTER;
    AutoLock lock(_lock);
    HRESULT hr = CheckShutdown();
    if (FAILED(hr)) return hr;
    return _pd->Clone(ppPD);
}

STDMETHODIMP MediaSource::GetCharacteristics(DWORD* pdwCharacteristics)
{
    if (!pdwCharacteristics)
        return E_POINTER;
    AutoLock lock(_lock);
    HRESULT hr = CheckShutdown();
    if (FAILED(hr)) return hr;
    *pdwCharacteristics = MFMEDIASOURCE_IS_LIVE;
    return S_OK;
}

STDMETHODIMP MediaSource::Pause()
{
    return MF_E_INVALID_STATE_TRANSITION;
}

STDMETHODIMP MediaSource::Start(IMFPresentationDescriptor* pPD, const GUID* pguidTimeFormat,
                                const PROPVARIANT* pvarStartPosition)
{
    if (pguidTimeFormat && *pguidTimeFormat != GUID_NULL)
        return MF_E_UNSUPPORTED_TIME_FORMAT;

    ComPtr<MediaStream> stream;
    bool firstStart = false;
    {
        AutoLock lock(_lock);
        HRESULT hr = CheckShutdown();
        if (FAILED(hr)) return hr;
        if (!pPD)
            return E_INVALIDARG;

        DWORD count = 0;
        hr = pPD->GetStreamDescriptorCount(&count);
        if (FAILED(hr) || count != 1)
            return MF_E_UNSUPPORTED_REPRESENTATION;

        BOOL selected = FALSE;
        ComPtr<IMFStreamDescriptor> sd;
        hr = pPD->GetStreamDescriptorByIndex(0, &selected, &sd);
        if (FAILED(hr) || !selected)
            return MF_E_UNSUPPORTED_REPRESENTATION;

        firstStart = !_streamNotified;
        _streamNotified = true;
        _state = State::Started;
        stream = _stream;

        hr = _queue->QueueEventParamUnk(firstStart ? MENewStream : MEUpdatedStream,
                                        GUID_NULL, S_OK,
                                        static_cast<IMFMediaStream2*>(stream.Get()));
        if (FAILED(hr)) return hr;
    }

    HRESULT hr = stream->Start(pvarStartPosition);
    if (FAILED(hr)) return hr;

    {
        AutoLock lock(_lock);
        hr = CheckShutdown();
        if (FAILED(hr)) return hr;
        hr = _queue->QueueEventParamVar(MESourceStarted, GUID_NULL, S_OK, pvarStartPosition);
    }
    VCamTrace(L"source: started (first=%d)", firstStart ? 1 : 0);
    return hr;
}

STDMETHODIMP MediaSource::Stop()
{
    ComPtr<MediaStream> stream;
    {
        AutoLock lock(_lock);
        HRESULT hr = CheckShutdown();
        if (FAILED(hr)) return hr;
        _state = State::Stopped;
        stream = _stream;
    }

    if (stream)
        stream->Stop();

    AutoLock lock(_lock);
    HRESULT hr = CheckShutdown();
    if (FAILED(hr)) return hr;
    VCamTrace(L"source: stopped");
    return _queue->QueueEventParamVar(MESourceStopped, GUID_NULL, S_OK, nullptr);
}

STDMETHODIMP MediaSource::Shutdown()
{
    AutoLock lock(_lock);
    if (_state == State::Shutdown)
        return MF_E_SHUTDOWN;
    _state = State::Shutdown;

    if (_stream)
        _stream->Shutdown();
    if (_queue)
        _queue->Shutdown();

    _stream.Reset();
    _pd.Reset();
    _attributes.Reset();
    VCamTrace(L"source: shutdown");
    return S_OK;
}

// ----- IMFMediaSourceEx --------------------------------------------------------

STDMETHODIMP MediaSource::GetSourceAttributes(IMFAttributes** ppAttributes)
{
    if (!ppAttributes)
        return E_POINTER;
    AutoLock lock(_lock);
    HRESULT hr = CheckShutdown();
    if (FAILED(hr)) return hr;
    *ppAttributes = _attributes.Get();
    (*ppAttributes)->AddRef();
    return S_OK;
}

STDMETHODIMP MediaSource::GetStreamAttributes(DWORD dwStreamIdentifier, IMFAttributes** ppAttributes)
{
    if (!ppAttributes)
        return E_POINTER;
    AutoLock lock(_lock);
    HRESULT hr = CheckShutdown();
    if (FAILED(hr)) return hr;
    if (dwStreamIdentifier != 0 || !_stream)
        return MF_E_INVALIDSTREAMNUMBER;
    *ppAttributes = _stream->Attributes();
    (*ppAttributes)->AddRef();
    return S_OK;
}

STDMETHODIMP MediaSource::SetD3DManager(IUnknown*)
{
    // Samples are system-memory; the Frame Server handles any GPU upload.
    return S_OK;
}

// ----- IMFGetService -------------------------------------------------------------

STDMETHODIMP MediaSource::GetService(REFGUID, REFIID, LPVOID* ppvObject)
{
    if (ppvObject)
        *ppvObject = nullptr;
    return MF_E_UNSUPPORTED_SERVICE;
}

// ----- IKsControl ----------------------------------------------------------------

STDMETHODIMP MediaSource::KsProperty(void*, ULONG, void*, ULONG, ULONG* BytesReturned)
{
    if (BytesReturned) *BytesReturned = 0;
    return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}
STDMETHODIMP MediaSource::KsMethod(void*, ULONG, void*, ULONG, ULONG* BytesReturned)
{
    if (BytesReturned) *BytesReturned = 0;
    return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}
STDMETHODIMP MediaSource::KsEvent(void*, ULONG, void*, ULONG, ULONG* BytesReturned)
{
    if (BytesReturned) *BytesReturned = 0;
    return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}
