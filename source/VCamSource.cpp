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

MediaStream::MediaStream(MediaSource* parent) : _parent(parent)
{
    DllAddRef();
}

namespace
{

void UpscaleNv12_2x(const uint8_t* src, uint8_t* dst, uint32_t srcW, uint32_t srcH)
{
    const uint8_t* srcY = src;
    const uint8_t* srcUV = src + srcW * srcH;
    
    uint8_t* dstY = dst;
    uint8_t* dstUV = dst + srcW * 2 * srcH * 2;
    
    // 1. Upscale Y plane (320x240 -> 640x480)
    for (uint32_t y = 0; y < srcH; ++y)
    {
        const uint8_t* srcRow = srcY + y * srcW;
        uint8_t* dstRow0 = dstY + (y * 2) * (srcW * 2);
        uint8_t* dstRow1 = dstRow0 + (srcW * 2);
        
        for (uint32_t x = 0; x < srcW; ++x)
        {
            uint8_t val = srcRow[x];
            dstRow0[x * 2]     = val;
            dstRow0[x * 2 + 1] = val;
            dstRow1[x * 2]     = val;
            dstRow1[x * 2 + 1] = val;
        }
    }
    
    // 2. Upscale UV plane (320x120 -> 640x240)
    for (uint32_t y = 0; y < srcH / 2; ++y)
    {
        const uint8_t* srcRowUV = srcUV + y * srcW;
        uint8_t* dstRowUV0 = dstUV + (y * 2) * (srcW * 2);
        uint8_t* dstRowUV1 = dstRowUV0 + (srcW * 2);
        
        for (uint32_t x = 0; x < srcW / 2; ++x)
        {
            uint8_t u = srcRowUV[x * 2];
            uint8_t v = srcRowUV[x * 2 + 1];
            
            dstRowUV0[x * 4]     = u;
            dstRowUV0[x * 4 + 1] = v;
            dstRowUV0[x * 4 + 2] = u;
            dstRowUV0[x * 4 + 3] = v;
            
            dstRowUV1[x * 4]     = u;
            dstRowUV1[x * 4 + 1] = v;
            dstRowUV1[x * 4 + 2] = u;
            dstRowUV1[x * 4 + 3] = v;
        }
    }
}

void DownscaleNv12_2x(const uint8_t* src, uint8_t* dst, uint32_t srcW, uint32_t srcH)
{
    const uint8_t* srcY = src;
    const uint8_t* srcUV = src + srcW * srcH;
    
    uint8_t* dstY = dst;
    uint8_t* dstUV = dst + (srcW / 2) * (srcH / 2);
    
    // 1. Downscale Y plane (640x480 -> 320x240)
    for (uint32_t y = 0; y < srcH / 2; ++y)
    {
        const uint8_t* srcRow = srcY + (y * 2) * srcW;
        uint8_t* dstRow = dstY + y * (srcW / 2);
        
        for (uint32_t x = 0; x < srcW / 2; ++x)
        {
            dstRow[x] = srcRow[x * 2];
        }
    }
    
    // 2. Downscale UV plane (640x240 -> 320x120)
    for (uint32_t y = 0; y < srcH / 4; ++y)
    {
        const uint8_t* srcRowUV = srcUV + (y * 2) * srcW;
        uint8_t* dstRowUV = dstUV + y * (srcW / 2);
        
        for (uint32_t x = 0; x < srcW / 4; ++x)
        {
            dstRowUV[x * 2]     = srcRowUV[x * 4];
            dstRowUV[x * 2 + 1] = srcRowUV[x * 4 + 1];
        }
    }
}

void Nv12ToYuy2(const uint8_t* src, uint8_t* dst, uint32_t w, uint32_t h)
{
    const uint8_t* srcY = src;
    const uint8_t* srcUV = src + w * h;
    
    for (uint32_t y = 0; y < h; ++y)
    {
        const uint8_t* rowY = srcY + y * w;
        const uint8_t* rowUV = srcUV + (y / 2) * w;
        uint32_t* rowDst = reinterpret_cast<uint32_t*>(dst + y * w * 2);
        
        for (uint32_t x = 0; x < w; x += 2)
        {
            uint32_t y0 = rowY[x];
            uint32_t y1 = rowY[x + 1];
            uint32_t u  = rowUV[x];
            uint32_t v  = rowUV[x + 1];
            
            rowDst[x / 2] = y0 | (u << 8) | (y1 << 16) | (v << 24);
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
    if (_bus.TryOpen() && _bus.ReadFormat(fmt))
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

    _staging.reset(new (std::nothrow) uint8_t[framebus::Nv12Bytes(_width, _height)]);
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
    // NV12 black: luma 0x10, chroma 0x80
    const uint32_t nv12Size = framebus::Nv12Bytes(_width, _height);
    memset(_staging.get(), 0x10, static_cast<size_t>(_width) * _height);
    memset(_staging.get() + static_cast<size_t>(_width) * _height, 0x80,
           static_cast<size_t>(nv12Size) - static_cast<size_t>(_width) * _height);
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
        if (!_ping.TryOpen())
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
    _state = MF_STREAM_STATE_RUNNING;
    _lastFrameId = 0;  // re-sync with whatever the bus currently holds

    ComPtr<IMFMediaTypeHandler> handler;
    if (SUCCEEDED(_descriptor->GetMediaTypeHandler(&handler)))
    {
        ComPtr<IMFMediaType> currentType;
        if (SUCCEEDED(handler->GetCurrentMediaType(&currentType)))
        {
            MFGetAttributeSize(currentType.Get(), MF_MT_FRAME_SIZE, &_width, &_height);
            MFGetAttributeRatio(currentType.Get(), MF_MT_FRAME_RATE, &_fpsNum, &_fpsDen);
            
            GUID subtype = MFVideoFormat_NV12;
            currentType->GetGUID(MF_MT_SUBTYPE, &subtype);
            _subtype = subtype;

            if (subtype == MFVideoFormat_YUY2)
            {
                _frameBytes = _width * _height * 2;
            }
            else
            {
                _frameBytes = framebus::Nv12Bytes(_width, _height);
            }
            _frameDuration = MulDiv(10000000, _fpsDen, _fpsNum);

            _staging.reset(new (std::nothrow) uint8_t[framebus::Nv12Bytes(_width, _height)]);
            if (!_staging)
                return E_OUTOFMEMORY;
            FillBlack();
            VCamTrace(L"stream: start with format %ux%u @ %u/%u (subtype YUY2=%d)", _width, _height, _fpsNum, _fpsDen, (subtype == MFVideoFormat_YUY2));
        }
    }

    PingActivity(true);
    VCamTrace(L"stream: started");
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
    // Pace delivery to the camera: wait (briefly) for a frame newer than the
    // last one we handed out. On timeout we re-deliver the previous frame so
    // the pipeline keeps flowing even if the host stalls or exits.
    const DWORD periodMs = static_cast<DWORD>(_frameDuration / 10000);
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
                if (_bus.TryOpen())
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
                const uint32_t busFrameBytes = framebus::Nv12Bytes(busWidth, busHeight);
                
                if (busWidth == _width && busHeight == _height)
                {
                    const LONG64 id = _bus.TryReadNewer(_staging.get(), busFrameBytes, _lastFrameId);
                    if (id != 0)
                    {
                        _lastFrameId = id;
                        break;
                    }
                }
                else
                {
                    const LONG64 id = _bus.TryReadNewer(_busStaging.get(), busFrameBytes, _lastFrameId);
                    if (id != 0)
                    {
                        _lastFrameId = id;
                        if (busWidth == 320 && busHeight == 240 && _width == 640 && _height == 480)
                        {
                            UpscaleNv12_2x(_busStaging.get(), _staging.get(), 320, 240);
                        }
                        else if (busWidth == 640 && busHeight == 480 && _width == 320 && _height == 240)
                        {
                            DownscaleNv12_2x(_busStaging.get(), _staging.get(), 640, 480);
                        }
                        else
                        {
                            FillBlack();
                        }
                        break;
                    }
                }
            }
        }
        if (GetTickCount64() >= deadline)
            break;
        Sleep(1);
    }

    ComPtr<IMFSample> sample;
    ComPtr<IMFMediaBuffer> buffer;
    HRESULT hr = MFCreateSample(&sample);
    if (SUCCEEDED(hr)) hr = MFCreateMemoryBuffer(_frameBytes, &buffer);
    if (SUCCEEDED(hr))
    {
        BYTE* data = nullptr;
        DWORD maxLen = 0;
        hr = buffer->Lock(&data, &maxLen, nullptr);
        if (SUCCEEDED(hr))
        {
            if (_subtype == MFVideoFormat_YUY2)
            {
                Nv12ToYuy2(_staging.get(), data, _width, _height);
            }
            else
            {
                memcpy(data, _staging.get(), _frameBytes);
            }
            buffer->Unlock();
            buffer->SetCurrentLength(_frameBytes);
        }
    }
    if (SUCCEEDED(hr)) hr = sample->AddBuffer(buffer.Get());
    if (SUCCEEDED(hr)) hr = sample->SetSampleTime(MFGetSystemTime());
    if (SUCCEEDED(hr)) hr = sample->SetSampleDuration(_frameDuration);
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

MediaSource::MediaSource()
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

    _stream.Attach(new (std::nothrow) MediaStream(this));
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
