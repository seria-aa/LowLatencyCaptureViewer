#pragma once

#include "capture/SampleGrabberCompat.h"

#include <dshow.h>

namespace llcv::capture {

// Owns every COM object and event handle used by one capture graph. Graph
// construction remains explicit in the application, while failure paths and
// normal shutdown share one deterministic release order.
class DirectShowGraphResources {
public:
    ~DirectShowGraphResources();

    DirectShowGraphResources() = default;
    DirectShowGraphResources(const DirectShowGraphResources&) = delete;
    DirectShowGraphResources& operator=(
        const DirectShowGraphResources&) = delete;

    void Reset();

    IGraphBuilder* graph = nullptr;
    IMediaControl* control = nullptr;
    IMediaFilter* mediaFilter = nullptr;
    IBaseFilter* capture = nullptr;
    IBaseFilter* audioCapture = nullptr;
    IPin* videoPin = nullptr;
    IPin* audioPin = nullptr;

    IBaseFilter* videoGrabberFilter = nullptr;
    ISampleGrabber* videoGrabber = nullptr;
    IBaseFilter* videoNullRenderer = nullptr;
    IPin* videoGrabberInput = nullptr;
    IPin* videoGrabberOutput = nullptr;
    IPin* videoNullInput = nullptr;
    ISampleGrabberCB* videoCallback = nullptr;

    IBaseFilter* audioGrabberFilter = nullptr;
    ISampleGrabber* audioGrabber = nullptr;
    IBaseFilter* audioNullRenderer = nullptr;
    IPin* audioGrabberInput = nullptr;
    IPin* audioGrabberOutput = nullptr;
    IPin* audioNullInput = nullptr;
    ISampleGrabberCB* audioCallback = nullptr;

    AM_MEDIA_TYPE* activeVideoType = nullptr;
    AM_MEDIA_TYPE* selectedAudioType = nullptr;
    HANDLE frameEvent = nullptr;
};

}  // namespace llcv::capture
