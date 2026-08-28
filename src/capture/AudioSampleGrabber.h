#pragma once

#include "audio/CaptureAudioFormat.h"
#include "audio/PcmPipeline.h"
#include "capture/SampleGrabberCompat.h"

#include <atomic>
#include <chrono>
#include <cstdint>

namespace llcv::capture {

struct AudioSampleTelemetry {
    std::atomic<uint64_t>* monitorStartMs = nullptr;
    std::atomic<UINT32>* packetFrames = nullptr;
    std::atomic<int64_t>* intervalUs = nullptr;
    std::atomic<uint64_t>* intervalTotalUs = nullptr;
    std::atomic<uint64_t>* lastCallbackMs = nullptr;
    std::atomic<uint64_t>* callbackCount = nullptr;
    std::atomic<uint64_t>* capturedFrames = nullptr;
};

class AudioSampleGrabberCallback final : public ISampleGrabberCB {
public:
    using TrackingPredicate = bool (*)(void* context);

    AudioSampleGrabberCallback(
        capture_audio::Format format, audio::PcmRing& ring,
        AudioSampleTelemetry telemetry,
        TrackingPredicate trackingPredicate = nullptr,
        void* trackingContext = nullptr) noexcept;

    STDMETHODIMP QueryInterface(REFIID id, void** object) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;
    STDMETHODIMP SampleCB(double sampleTime, IMediaSample* sample) override;
    STDMETHODIMP BufferCB(double sampleTime, BYTE* buffer,
                          long bufferLength) override;

private:
    bool TrackingActive() const noexcept;

    std::atomic<ULONG> references_{1};
    capture_audio::Format format_{};
    audio::PcmRing& ring_;
    AudioSampleTelemetry telemetry_{};
    TrackingPredicate trackingPredicate_ = nullptr;
    void* trackingContext_ = nullptr;
    std::chrono::steady_clock::time_point previousCallback_{};
    bool hasPreviousCallback_ = false;
};

}  // namespace llcv::capture
