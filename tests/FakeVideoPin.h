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
        DWORD controlFlags = 0;
    };
    std::vector<Mode> modes;
    std::vector<int> attemptedRates;
    int rejectedRate = 0;
    DWORD rejectedControlFlags = 0;
    int formatQueries = 0;
    int streamCapsQueries = 0;
    bool countFails = false;
    bool negativeCapabilityCount = false;
    bool allocateOnQueryFailure = false;
    // Only inspected on an unbalanced reference count by the fault test, so
    // it can clean up an intentionally reproduced pre-fix allocation leak.
    AM_MEDIA_TYPE* lastFailedMedia = nullptr;
    bool nullActiveFormat = false;
    bool echoAcceptedFormat = false;
    bool stripReadbackColor = false;
    bool legacyReadback = false;
    VIDEOINFOHEADER2 acceptedInfo{};
    GUID acceptedSubtype{};
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
        *count = negativeCapabilityCount ? -1 : static_cast<int>(modes.size());
        *bytes = sizeof(VIDEO_STREAM_CONFIG_CAPS);
        return S_OK;
    }
    STDMETHODIMP GetStreamCaps(int index, AM_MEDIA_TYPE** output, BYTE* buffer) override {
        ++streamCapsQueries;
        *output = nullptr;
        if (index < 0 || index >= static_cast<int>(modes.size())) return E_INVALIDARG;
        const auto& mode = modes[index];
        if (mode.queryFails && !allocateOnQueryFailure) return E_FAIL;
        auto* media = static_cast<AM_MEDIA_TYPE*>(CoTaskMemAlloc(sizeof(AM_MEDIA_TYPE)));
        auto* info = static_cast<VIDEOINFOHEADER2*>(CoTaskMemAlloc(sizeof(VIDEOINFOHEADER2)));
        if (!media || !info) {
            CoTaskMemFree(media); CoTaskMemFree(info); return E_OUTOFMEMORY;
        }
        *media = {}; *info = {};
        info->AvgTimePerFrame = mode.fps ? 10'000'000 / mode.fps : 0;
        info->dwControlFlags = mode.controlFlags;
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
        if (mode.queryFails) {
            // Failure HRESULTs do not guarantee a null out-parameter. Attach
            // a counted COM reference to make release of the media type
            // observable without changing ordinary fake-driver behavior.
            media->pUnk = static_cast<IPin*>(this);
            AddRef();
            lastFailedMedia = media;
            return E_FAIL;
        }
        return S_OK;
    }
    STDMETHODIMP SetFormat(AM_MEDIA_TYPE* media) override {
        const auto* info = reinterpret_cast<const VIDEOINFOHEADER2*>(media->pbFormat);
        const int fps = static_cast<int>((10'000'000 + info->AvgTimePerFrame / 2) /
                                         info->AvgTimePerFrame);
        attemptedRates.push_back(fps);
        const bool rejected = fps == rejectedRate || (rejectedControlFlags != 0 &&
            info->dwControlFlags == rejectedControlFlags);
        if (!rejected) { acceptedInfo = *info; acceptedSubtype = media->subtype; }
        return rejected ? E_FAIL : S_OK;
    }
    STDMETHODIMP GetFormat(AM_MEDIA_TYPE** media) override {
        ++formatQueries;
        if (nullActiveFormat) { *media = nullptr; return S_OK; }
        BYTE caps[sizeof(VIDEO_STREAM_CONFIG_CAPS)]{};
        const HRESULT hr = GetStreamCaps(0, media, caps);
        if (SUCCEEDED(hr) && echoAcceptedFormat && acceptedInfo.AvgTimePerFrame) {
            *reinterpret_cast<VIDEOINFOHEADER2*>((*media)->pbFormat) = acceptedInfo;
            (*media)->subtype = acceptedSubtype;
        }
        if (SUCCEEDED(hr) && (*media)->pbFormat) {
            auto* info = reinterpret_cast<VIDEOINFOHEADER2*>((*media)->pbFormat);
            if (stripReadbackColor) info->dwControlFlags = 0;
            if (legacyReadback) {
                // Model a driver that exposes color flags only in its caps.
                VIDEOINFOHEADER legacy{};
                legacy.rcSource = info->rcSource;
                legacy.rcTarget = info->rcTarget;
                legacy.dwBitRate = info->dwBitRate;
                legacy.dwBitErrorRate = info->dwBitErrorRate;
                legacy.AvgTimePerFrame = info->AvgTimePerFrame;
                legacy.bmiHeader = info->bmiHeader;
                std::memcpy((*media)->pbFormat, &legacy, sizeof(legacy));
                (*media)->formattype = FORMAT_VideoInfo;
                (*media)->cbFormat = sizeof(legacy);
            }
        }
        return hr;
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
