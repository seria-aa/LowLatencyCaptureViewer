#pragma once

#include "audio/CaptureAudioFormat.h"

#include <windows.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

namespace llcv::audio {

// Fixed-stereo PCM mailbox with bounded storage. Capture never waits for
// render: when full, the oldest frames are discarded.
class PcmRing {
public:
    using OverrunObserver = bool (*)(void* context, size_t droppedFrames);

    explicit PcmRing(size_t capacityFrames,
                     std::atomic<UINT32>* publishedFrames = nullptr,
                     OverrunObserver overrunObserver = nullptr,
                     void* observerContext = nullptr);

    void Push(const int16_t* samples, size_t frames);
    void PushConverted(const BYTE* source, size_t frames,
                       const capture_audio::Format& format);
    size_t Pop(int16_t* output, size_t frames);
    size_t AvailableFrames() const;
    void Clear();
    uint64_t Overruns() const noexcept;

private:
    static constexpr size_t kChannels = 2;

    size_t PrepareWrite(size_t frames);
    void PublishAvailable() noexcept;

    const size_t capacityFrames_;
    std::vector<int16_t> data_;
    mutable std::mutex mutex_;
    size_t readFrame_ = 0;
    size_t writeFrame_ = 0;
    size_t available_ = 0;
    std::atomic<uint64_t> overruns_{0};
    std::atomic<UINT32>* publishedFrames_ = nullptr;
    OverrunObserver overrunObserver_ = nullptr;
    void* observerContext_ = nullptr;
};

// Bounded windowed-sinc drift corrector. With correction disabled callers
// bypass this class entirely, preserving the integer PCM path.
class SincDriftResampler {
public:
    explicit SincDriftResampler(
        PcmRing& ring,
        std::atomic<UINT32>* publishedBufferedFrames = nullptr) noexcept;

    void Prepare(size_t maxOutputFrames);
    void Reset();
    size_t Render(int16_t* output, size_t outputFrames, double ratio);
    size_t BufferedFrames() const noexcept;

private:
    static constexpr size_t kChannels = 2;
    static constexpr int kHalfTaps = 8;
    static constexpr int kHistoryFrames = kHalfTaps * 2;
    static constexpr double kPi = 3.14159265358979323846;

    static double Sinc(double value) noexcept;
    static double WindowedSinc(double distance) noexcept;
    size_t AppendFromRing(size_t wantedFrames);
    void CompactHistory();
    void PublishBuffered() noexcept;

    PcmRing& ring_;
    std::atomic<UINT32>* publishedBufferedFrames_ = nullptr;
    std::vector<int16_t> source_;
    std::vector<int16_t> transfer_;
    double position_ = 0.0;
    bool primed_ = false;
};

}  // namespace llcv::audio
