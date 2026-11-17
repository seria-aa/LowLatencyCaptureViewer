#include "capture/DirectShowGraphResources.h"
#include "capture/LatestVideoSample.h"

namespace llcv::capture {
namespace {

template<class T>
void SafeRelease(T*& value) {
    if (value) {
        value->Release();
        value = nullptr;
    }
}

void DeleteMediaType(AM_MEDIA_TYPE*& mediaType) {
    if (!mediaType) return;
    if (mediaType->cbFormat != 0) {
        CoTaskMemFree(mediaType->pbFormat);
        mediaType->cbFormat = 0;
        mediaType->pbFormat = nullptr;
    }
    SafeRelease(mediaType->pUnk);
    CoTaskMemFree(mediaType);
    mediaType = nullptr;
}

}  // namespace

DirectShowGraphResources::DirectShowGraphResources() = default;

DirectShowGraphResources::~DirectShowGraphResources() {
    Reset();
}

HRESULT DirectShowGraphResources::RemoveAudioBranch() {
    if (audioGrabber) audioGrabber->SetCallback(nullptr, 0);
    SafeRelease(audioCallback);
    SafeRelease(audioNullInput);
    SafeRelease(audioGrabberOutput);
    SafeRelease(audioGrabberInput);
    SafeRelease(audioGrabber);
    SafeRelease(audioPin);
    DeleteMediaType(selectedAudioType);
    HRESULT result = S_OK;
    // RemoveFilter disconnects the filter's pins first. A filter whose
    // AddFilter failed has no graph and is only released. A filter that
    // cannot say whether it is in the graph may still be there: that is a
    // failure, never a successful removal.
    const auto remove = [&](IBaseFilter*& filter) {
        if (!filter) return;
        if (graph && filter != capture) {
            FILTER_INFO info{};
            HRESULT hr = filter->QueryFilterInfo(&info);
            const bool inGraph = SUCCEEDED(hr) && info.pGraph;
            SafeRelease(info.pGraph);
            if (inGraph) hr = graph->RemoveFilter(filter);
            if (FAILED(hr) && SUCCEEDED(result)) result = hr;
        }
        SafeRelease(filter);
    };
    remove(audioNullRenderer);
    remove(audioGrabberFilter);
    remove(audioCapture);
    return result;
}

void DirectShowGraphResources::Reset() {
    if (control) control->Stop();
    events.Reset();
    if (audioGrabber) audioGrabber->SetCallback(nullptr, 0);
    SafeRelease(audioCallback);
    if (videoGrabber) videoGrabber->SetCallback(nullptr, 0);
    SafeRelease(videoCallback);
    latestVideoSample.reset();
    if (frameEvent) {
        CloseHandle(frameEvent);
        frameEvent = nullptr;
    }

    SafeRelease(videoNullInput);
    SafeRelease(audioNullInput);
    SafeRelease(audioGrabberOutput);
    SafeRelease(audioGrabberInput);
    SafeRelease(audioNullRenderer);
    SafeRelease(audioGrabber);
    SafeRelease(audioGrabberFilter);
    SafeRelease(videoGrabberOutput);
    SafeRelease(videoGrabberInput);
    SafeRelease(videoNullRenderer);
    SafeRelease(videoGrabber);
    SafeRelease(videoGrabberFilter);
    DeleteMediaType(activeVideoType);
    DeleteMediaType(selectedAudioType);
    SafeRelease(videoPin);
    SafeRelease(audioPin);
    SafeRelease(audioCapture);
    SafeRelease(capture);
    SafeRelease(mediaFilter);
    SafeRelease(control);
    SafeRelease(graph);
}

}  // namespace llcv::capture
