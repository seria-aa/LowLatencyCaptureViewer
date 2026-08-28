#pragma once

#include "capture/SampleGrabberCompat.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>

namespace llcv::capture {

struct VideoSampleTelemetry {
    const std::atomic<uint64_t>* trackingStartMilliseconds = nullptr;
    std::atomic<uint64_t>* capturedFrames = nullptr;
    std::atomic<uint64_t>* replacedFrames = nullptr;
};

// A single-slot mailbox between the DirectShow callback and render thread.
// A newly captured frame replaces an unconsumed older frame, preventing
// application-side video latency from accumulating.
class LatestVideoSample {
public:
    LatestVideoSample(
        size_t expectedBytes, HANDLE readyEvent,
        VideoSampleTelemetry telemetry = {});
    ~LatestVideoSample();

    LatestVideoSample(const LatestVideoSample&) = delete;
    LatestVideoSample& operator=(const LatestVideoSample&) = delete;

    void Push(IMediaSample* sample);
    IMediaSample* TakeLatest(int64_t& arrivalMicroseconds);

private:
    bool TrackingActive() const;

    std::mutex mutex_;
    IMediaSample* latest_ = nullptr;
    size_t expectedBytes_ = 0;
    HANDLE readyEvent_ = nullptr;
    int64_t latestArrivalMicroseconds_ = 0;
    VideoSampleTelemetry telemetry_{};
};

class VideoSampleGrabberCallback final : public ISampleGrabberCB {
public:
    explicit VideoSampleGrabberCallback(LatestVideoSample* sampleSlot);

    STDMETHODIMP QueryInterface(REFIID id, void** object) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;
    STDMETHODIMP SampleCB(double sampleTime, IMediaSample* sample) override;
    STDMETHODIMP BufferCB(
        double sampleTime, BYTE* buffer, long bufferLength) override;

private:
    std::atomic<ULONG> references_{1};
    std::atomic<bool> surfaceCapabilityProbed_{false};
    LatestVideoSample* sampleSlot_ = nullptr;
};

}  // namespace llcv::capture
