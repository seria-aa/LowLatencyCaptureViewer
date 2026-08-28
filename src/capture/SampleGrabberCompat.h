#pragma once

#include <dshow.h>

// qedit.h is no longer shipped in current Windows SDKs. These are the
// original Sample Grabber declarations required to use the in-box legacy
// DirectShow component without taking a dependency on an obsolete header.
struct __declspec(uuid("0579154A-2B53-4994-B0D0-E773148EFF85"))
    ISampleGrabberCB : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE SampleCB(
        double sampleTime, IMediaSample* sample) = 0;
    virtual HRESULT STDMETHODCALLTYPE BufferCB(
        double sampleTime, BYTE* buffer, long bufferLength) = 0;
};

struct __declspec(uuid("6B652FFF-11FE-4FCE-92AD-0266B5D7C78F"))
    ISampleGrabber : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE SetOneShot(BOOL oneShot) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetMediaType(
        const AM_MEDIA_TYPE* mediaType) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetConnectedMediaType(
        AM_MEDIA_TYPE* mediaType) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetBufferSamples(BOOL bufferThem) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetCurrentBuffer(
        long* bufferSize, long* buffer) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetCurrentSample(
        IMediaSample** sample) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetCallback(
        ISampleGrabberCB* callback, long callbackMethod) = 0;
};

inline constexpr CLSID kSampleGrabberClassId = {
    0xC1F400A0, 0x3F08, 0x11D3,
    {0x9F, 0x0B, 0x00, 0x60, 0x08, 0x03, 0x9E, 0x37}};

inline constexpr CLSID kNullRendererClassId = {
    0xC1F400A4, 0x3F08, 0x11D3,
    {0x9F, 0x0B, 0x00, 0x60, 0x08, 0x03, 0x9E, 0x37}};
