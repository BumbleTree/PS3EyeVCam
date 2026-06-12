#pragma once
//
// PS3EyeVCam.dll — Media Foundation custom media source for the Windows 11
// virtual camera (MFCreateVirtualCamera) pipeline.
//
// These objects are instantiated inside the Camera Frame Server service, not
// in the host app. They read NV12 frames from the FrameBus shared-memory
// section published by PS3EyeVCamHost.exe.
//
// Built against SDK 10.0.19041: every interface used here (IMFMediaSourceEx,
// IMFMediaStream2, the MF_DEVICESTREAM_* attributes) exists in that SDK; only
// the host-side MFCreateVirtualCamera API needed a local declaration.
//
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mferror.h>
#include <wrl/client.h>
#include <memory>

#include "../common/VCamGuids.h"
#include "../common/FrameBus.h"
#include "../common/ControlBus.h"
#include "../common/Settings.h"
#include <vector>

using Microsoft::WRL::ComPtr;

// ---------------------------------------------------------------------------
// IKsControl, declared locally so we don't need ks.h/ksproxy.h. The IID and
// vtable match the SDK declaration; parameters are opaque because we answer
// every request with ERROR_SET_NOT_FOUND (the documented "no such property
// set" response, which the Frame Server requires rather than E_NOTIMPL).
// ---------------------------------------------------------------------------
MIDL_INTERFACE("28F54685-06FD-11D2-B27A-00A0C9223196")
IKsControl : public IUnknown
{
    virtual HRESULT STDMETHODCALLTYPE KsProperty(void* Property, ULONG PropertyLength,
                                                 void* PropertyData, ULONG DataLength,
                                                 ULONG* BytesReturned) = 0;
    virtual HRESULT STDMETHODCALLTYPE KsMethod(void* Method, ULONG MethodLength,
                                               void* MethodData, ULONG DataLength,
                                               ULONG* BytesReturned) = 0;
    virtual HRESULT STDMETHODCALLTYPE KsEvent(void* Event, ULONG EventLength,
                                              void* EventData, ULONG DataLength,
                                              ULONG* BytesReturned) = 0;
};

// PINNAME_VIDEO_CAPTURE (== PINNAME_CAPTURE) from ksmedia.h, declared locally
// for the same reason.
inline constexpr GUID kPinCategoryCapture =
    { 0xfb6c4281, 0x0353, 0x11d1, { 0x90, 0x5f, 0x00, 0x00, 0xc0, 0xcc, 0x16, 0xba } };

// MFFrameSourceTypes_Color (mfapi.h enum value)
inline constexpr UINT32 kFrameSourceTypeColor = 0x0001;

void VCamTrace(const wchar_t* fmt, ...);

// Module-lifetime bookkeeping for DllCanUnloadNow.
void DllAddRef();
void DllRelease();

class CritSec
{
public:
    CritSec()  { InitializeCriticalSection(&_cs); }
    ~CritSec() { DeleteCriticalSection(&_cs); }
    void Lock()   { EnterCriticalSection(&_cs); }
    void Unlock() { LeaveCriticalSection(&_cs); }
private:
    CRITICAL_SECTION _cs;
};

class AutoLock
{
public:
    explicit AutoLock(CritSec& cs) : _cs(cs) { _cs.Lock(); }
    ~AutoLock() { _cs.Unlock(); }
private:
    CritSec& _cs;
};

class MediaSource;

// ---------------------------------------------------------------------------
// MediaStream — the single video stream (stream id 0).
// ---------------------------------------------------------------------------
class MediaStream final : public IMFMediaStream2, public IKsControl
{
public:
    explicit MediaStream(MediaSource* parent);
    virtual ~MediaStream() = default;

    HRESULT Initialize();

    // Called by MediaSource with the source lock NOT held.
    HRESULT Start(const PROPVARIANT* startPosition);
    HRESULT Stop();
    HRESULT Shutdown();

    IMFStreamDescriptor* Descriptor() { return _descriptor.Get(); }
    IMFAttributes* Attributes() { return _attributes.Get(); }

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;

    // IMFMediaEventGenerator
    STDMETHODIMP BeginGetEvent(IMFAsyncCallback* pCallback, IUnknown* punkState) override;
    STDMETHODIMP EndGetEvent(IMFAsyncResult* pResult, IMFMediaEvent** ppEvent) override;
    STDMETHODIMP GetEvent(DWORD dwFlags, IMFMediaEvent** ppEvent) override;
    STDMETHODIMP QueueEvent(MediaEventType met, REFGUID guidExtendedType,
                            HRESULT hrStatus, const PROPVARIANT* pvValue) override;

    // IMFMediaStream
    STDMETHODIMP GetMediaSource(IMFMediaSource** ppMediaSource) override;
    STDMETHODIMP GetStreamDescriptor(IMFStreamDescriptor** ppStreamDescriptor) override;
    STDMETHODIMP RequestSample(IUnknown* pToken) override;

    // IMFMediaStream2
    STDMETHODIMP SetStreamState(MF_STREAM_STATE value) override;
    STDMETHODIMP GetStreamState(MF_STREAM_STATE* value) override;

    // IKsControl
    STDMETHODIMP KsProperty(void*, ULONG, void*, ULONG, ULONG*) override;
    STDMETHODIMP KsMethod(void*, ULONG, void*, ULONG, ULONG*) override;
    STDMETHODIMP KsEvent(void*, ULONG, void*, ULONG, ULONG*) override;

private:
    HRESULT CreateMediaType(uint32_t width, uint32_t height, uint32_t fpsNum, uint32_t fpsDen, GUID subtype, IMFMediaType** ppType);
    HRESULT DeliverSample(IUnknown* token);
    void    FillBlack();
    void    PingActivity(bool forceWakeSignal);  // caller must hold _lock

    LONG    _refCount = 1;
    CritSec _lock;

    MediaSource*               _parent;       // weak during construction...
    ComPtr<IMFMediaSource>     _parentRef;    // ...strong once initialized; dropped at Shutdown
    ComPtr<IMFMediaEventQueue> _queue;
    ComPtr<IMFStreamDescriptor> _descriptor;
    ComPtr<IMFAttributes>      _attributes;

    MF_STREAM_STATE _state    = MF_STREAM_STATE_STOPPED;
    bool            _shutdown = false;

    // Negotiated/advertised format (fixed for the lifetime of the source).
    GUID     _subtype = MFVideoFormat_NV12;
    uint32_t _width = 640, _height = 480, _fpsNum = 60, _fpsDen = 1;
    uint32_t _frameBytes = 0;
    LONGLONG _frameDuration = 166667;  // 100ns units

    framebus::Reader           _bus;
    ULONGLONG                  _nextBusRetryTick = 0;
    LONG64                     _lastFrameId = 0;
    std::unique_ptr<uint8_t[]> _staging;   // last good frame (starts black)
    std::unique_ptr<uint8_t[]> _busStaging; // temp buffer for reading raw frames from the bus

    // Sleep/wake keepalive towards the host. Closed by the destructor, not by
    // Shutdown (same in-flight-call rationale as _bus).
    controlbus::Pinger _ping;
    ULONGLONG          _nextPingRetryTick = 0;
    ULONGLONG          _lastWakeSignalTick = 0;
};

// ---------------------------------------------------------------------------
// MediaSource
// ---------------------------------------------------------------------------
class MediaSource final : public IMFMediaSourceEx, public IMFGetService, public IKsControl
{
public:
    MediaSource();
    virtual ~MediaSource() = default;

    HRESULT Initialize(IMFAttributes* activationAttributes);

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;

    // IMFMediaEventGenerator
    STDMETHODIMP BeginGetEvent(IMFAsyncCallback* pCallback, IUnknown* punkState) override;
    STDMETHODIMP EndGetEvent(IMFAsyncResult* pResult, IMFMediaEvent** ppEvent) override;
    STDMETHODIMP GetEvent(DWORD dwFlags, IMFMediaEvent** ppEvent) override;
    STDMETHODIMP QueueEvent(MediaEventType met, REFGUID guidExtendedType,
                            HRESULT hrStatus, const PROPVARIANT* pvValue) override;

    // IMFMediaSource
    STDMETHODIMP CreatePresentationDescriptor(IMFPresentationDescriptor** ppPD) override;
    STDMETHODIMP GetCharacteristics(DWORD* pdwCharacteristics) override;
    STDMETHODIMP Pause() override;
    STDMETHODIMP Shutdown() override;
    STDMETHODIMP Start(IMFPresentationDescriptor* pPD, const GUID* pguidTimeFormat,
                       const PROPVARIANT* pvarStartPosition) override;
    STDMETHODIMP Stop() override;

    // IMFMediaSourceEx
    STDMETHODIMP GetSourceAttributes(IMFAttributes** ppAttributes) override;
    STDMETHODIMP GetStreamAttributes(DWORD dwStreamIdentifier, IMFAttributes** ppAttributes) override;
    STDMETHODIMP SetD3DManager(IUnknown* pManager) override;

    // IMFGetService
    STDMETHODIMP GetService(REFGUID guidService, REFIID riid, LPVOID* ppvObject) override;

    // IKsControl
    STDMETHODIMP KsProperty(void*, ULONG, void*, ULONG, ULONG*) override;
    STDMETHODIMP KsMethod(void*, ULONG, void*, ULONG, ULONG*) override;
    STDMETHODIMP KsEvent(void*, ULONG, void*, ULONG, ULONG*) override;

private:
    enum class State { Stopped, Started, Shutdown };

    HRESULT CheckShutdown() const
    {
        return _state == State::Shutdown ? MF_E_SHUTDOWN : S_OK;
    }

    LONG    _refCount = 1;
    CritSec _lock;

    State _state = State::Stopped;
    bool  _streamNotified = false;  // MENewStream already sent once

    ComPtr<IMFMediaEventQueue>         _queue;
    ComPtr<IMFAttributes>              _attributes;
    ComPtr<MediaStream>                _stream;
    ComPtr<IMFPresentationDescriptor>  _pd;
};
