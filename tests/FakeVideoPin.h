#pragma once

#include <dshow.h>
#include <dvdmedia.h>
#include <algorithm>
#include <cstring>
#include <vector>

// Test-only driver boundary. No hardware, registry or persistent settings.
class FakeVideoPin final : public IPin, public IAMStreamConfig {
public:
    struct Mode {
        int fps;
        int width = 1920;
        int height = 1080;
        int maximumFps = 0;
        bool queryFails = false;
        GUID subtype = MEDIASUBTYPE_NV12;
        long imageBytes = -1;
        bool emptyFormat = false;
        REFERENCE_TIME minimumInterval = 0;
        REFERENCE_TIME maximumInterval = 0;
    };
    std::vector<Mode> modes;
    std::vector<int> attemptedRates;
    int rejectedRate = 0;
    bool countFails = false;
    bool nullActiveFormat = false;
    ULONG references = 1;

    STDMETHODIMP QueryInterface(REFIID iid, void** object) override {
        if (!object) return E_POINTER;
        *object = nullptr;
        if (iid == IID_IUnknown || iid == IID_IPin)
            *object = static_cast<IPin*>(this);
        else if (iid == IID_IAMStreamConfig)
            *object = static_cast<IAMStreamConfig*>(this);
        else return E_NOINTERFACE;
        AddRef(); return S_OK;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++references; }
    STDMETHODIMP_(ULONG) Release() override { return --references; }
    STDMETHODIMP GetNumberOfCapabilities(int* count, int* bytes) override {
        if (countFails) return E_FAIL;
        *count = static_cast<int>(modes.size());
        *bytes = sizeof(VIDEO_STREAM_CONFIG_CAPS);
        return S_OK;
    }
    STDMETHODIMP GetStreamCaps(int index, AM_MEDIA_TYPE** output, BYTE* buffer) override {
        *output = nullptr;
        if (index < 0 || index >= static_cast<int>(modes.size())) return E_INVALIDARG;
        const auto& mode = modes[index];
        if (mode.queryFails) return E_FAIL;
        auto* media = static_cast<AM_MEDIA_TYPE*>(CoTaskMemAlloc(sizeof(AM_MEDIA_TYPE)));
        auto* info = static_cast<VIDEOINFOHEADER2*>(CoTaskMemAlloc(sizeof(VIDEOINFOHEADER2)));
        if (!media || !info) {
            CoTaskMemFree(media); CoTaskMemFree(info); return E_OUTOFMEMORY;
        }
        *media = {}; *info = {};
        info->AvgTimePerFrame = mode.fps ? 10'000'000 / mode.fps : 0;
        info->bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info->bmiHeader.biWidth = mode.width;
        info->bmiHeader.biHeight = mode.height;
        info->bmiHeader.biSizeImage = mode.imageBytes >= 0
            ? mode.imageBytes : static_cast<DWORD>(int64_t(mode.width) * mode.height * 3 / 2);
        media->majortype = MEDIATYPE_Video;
        media->subtype = mode.subtype;
        media->formattype = FORMAT_VideoInfo2;
        media->cbFormat = sizeof(*info);
        media->pbFormat = reinterpret_cast<BYTE*>(info);
        VIDEO_STREAM_CONFIG_CAPS caps{};
        const int maximum = mode.maximumFps ? mode.maximumFps : mode.fps;
        caps.MinFrameInterval = maximum ? 10'000'000 / maximum : 0;
        caps.MaxFrameInterval = info->AvgTimePerFrame;
        if (mode.minimumInterval) caps.MinFrameInterval = mode.minimumInterval;
        if (mode.maximumInterval) caps.MaxFrameInterval = mode.maximumInterval;
        std::memcpy(buffer, &caps, sizeof(caps));
        if (mode.emptyFormat) {
            CoTaskMemFree(media->pbFormat);
            media->pbFormat = nullptr;
            media->cbFormat = 0;
        }
        *output = media;
        return S_OK;
    }
    STDMETHODIMP SetFormat(AM_MEDIA_TYPE* media) override {
        const auto* info = reinterpret_cast<const VIDEOINFOHEADER2*>(media->pbFormat);
        const int fps = static_cast<int>((10'000'000 + info->AvgTimePerFrame / 2) /
                                         info->AvgTimePerFrame);
        attemptedRates.push_back(fps);
        return fps == rejectedRate ? E_FAIL : S_OK;
    }
    STDMETHODIMP GetFormat(AM_MEDIA_TYPE** media) override {
        if (nullActiveFormat) { *media = nullptr; return S_OK; }
        BYTE caps[sizeof(VIDEO_STREAM_CONFIG_CAPS)]{};
        return GetStreamCaps(0, media, caps);
    }
    STDMETHODIMP Connect(IPin*, const AM_MEDIA_TYPE*) override { return E_NOTIMPL; }
    STDMETHODIMP ReceiveConnection(IPin*, const AM_MEDIA_TYPE*) override { return E_NOTIMPL; }
    STDMETHODIMP Disconnect() override { return E_NOTIMPL; }
    STDMETHODIMP ConnectedTo(IPin**) override { return E_NOTIMPL; }
    STDMETHODIMP ConnectionMediaType(AM_MEDIA_TYPE*) override { return E_NOTIMPL; }
    STDMETHODIMP QueryPinInfo(PIN_INFO*) override { return E_NOTIMPL; }
    STDMETHODIMP QueryDirection(PIN_DIRECTION*) override { return E_NOTIMPL; }
    STDMETHODIMP QueryId(LPWSTR*) override { return E_NOTIMPL; }
    STDMETHODIMP QueryAccept(const AM_MEDIA_TYPE*) override { return E_NOTIMPL; }
    STDMETHODIMP EnumMediaTypes(IEnumMediaTypes**) override { return E_NOTIMPL; }
    STDMETHODIMP QueryInternalConnections(IPin**, ULONG*) override { return E_NOTIMPL; }
    STDMETHODIMP EndOfStream() override { return E_NOTIMPL; }
    STDMETHODIMP BeginFlush() override { return E_NOTIMPL; }
    STDMETHODIMP EndFlush() override { return E_NOTIMPL; }
    STDMETHODIMP NewSegment(REFERENCE_TIME, REFERENCE_TIME, double) override { return E_NOTIMPL; }
};
