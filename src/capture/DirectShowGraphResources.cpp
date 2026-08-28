#include "capture/DirectShowGraphResources.h"

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

DirectShowGraphResources::~DirectShowGraphResources() {
    Reset();
}

void DirectShowGraphResources::Reset() {
    if (control) control->Stop();
    if (audioGrabber) audioGrabber->SetCallback(nullptr, 0);
    SafeRelease(audioCallback);
    if (videoGrabber) videoGrabber->SetCallback(nullptr, 0);
    SafeRelease(videoCallback);
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
