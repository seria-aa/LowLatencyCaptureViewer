#pragma once

#include "capture/SampleGrabberCompat.h"
#include "capture/CaptureGraphEvents.h"

#include <dshow.h>
#include <memory>

namespace llcv::capture {

class LatestVideoSample;

// Owns every COM object and event handle used by one capture graph. Graph
// construction remains explicit in the application, while failure paths and
// normal shutdown share one deterministic release order.
class DirectShowGraphResources {
public:
    ~DirectShowGraphResources();

    DirectShowGraphResources();
    DirectShowGraphResources(const DirectShowGraphResources&) = delete;
    DirectShowGraphResources& operator=(
        const DirectShowGraphResources&) = delete;

    void Reset();
    // Detaches a partly or fully built audio branch so the video branch can
    // start alone: releases every audio object and removes the audio grabber,
    // audio null renderer and a separate audio capture filter from the graph.
    // An audio pin on the video capture filter is only released; that filter
    // stays. Fails when a filter could not be removed; the graph must not be
    // started then.
    HRESULT RemoveAudioBranch();

    IGraphBuilder* graph = nullptr;
    IMediaControl* control = nullptr;
    IMediaFilter* mediaFilter = nullptr;
    CaptureGraphEvents events;
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
    // Outlives registered callbacks, including partially failed graph starts.
    std::unique_ptr<LatestVideoSample> latestVideoSample;

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
