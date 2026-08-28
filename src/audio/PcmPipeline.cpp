#include "audio/PcmPipeline.h"

#include <algorithm>
#include <cmath>
#include <climits>
#include <limits>

namespace llcv::audio {

PcmRing::PcmRing(size_t capacityFrames,
                 std::atomic<UINT32>* publishedFrames,
                 OverrunObserver overrunObserver, void* observerContext)
    : capacityFrames_((std::max)(size_t{1}, capacityFrames)),
      data_(capacityFrames_ * kChannels),
      publishedFrames_(publishedFrames),
      overrunObserver_(overrunObserver),
      observerContext_(observerContext) {
    PublishAvailable();
}

size_t PcmRing::PrepareWrite(size_t frames) {
    if (frames >= capacityFrames_) {
        readFrame_ = 0;
        writeFrame_ = 0;
        available_ = 0;
        return capacityFrames_;
    }

    while (available_ + frames > capacityFrames_) {
        const size_t drop = (std::min)(available_ + frames - capacityFrames_,
                                      available_);
        readFrame_ = (readFrame_ + drop) % capacityFrames_;
        available_ -= drop;
        if (overrunObserver_ &&
            overrunObserver_(observerContext_, drop)) {
            overruns_.fetch_add(1, std::memory_order_relaxed);
        }
    }
    return frames;
}

void PcmRing::Push(const int16_t* samples, size_t frames) {
    if (!samples || frames == 0) return;
    std::lock_guard<std::mutex> lock(mutex_);
    if (frames >= capacityFrames_) {
        samples += (frames - capacityFrames_) * kChannels;
    }
    frames = PrepareWrite(frames);
    for (size_t i = 0; i < frames; ++i) {
        const size_t destination =
            ((writeFrame_ + i) % capacityFrames_) * kChannels;
        const size_t source = i * kChannels;
        data_[destination] = samples[source];
        data_[destination + 1] = samples[source + 1];
    }
    writeFrame_ = (writeFrame_ + frames) % capacityFrames_;
    available_ += frames;
    PublishAvailable();
}

void PcmRing::PushConverted(const BYTE* source, size_t frames,
                            const capture_audio::Format& format) {
    if (!source || frames == 0 || format.blockAlign == 0) return;
    std::lock_guard<std::mutex> lock(mutex_);
    if (frames >= capacityFrames_) {
        source += (frames - capacityFrames_) * format.blockAlign;
    }
    frames = PrepareWrite(frames);
    for (size_t i = 0; i < frames; ++i) {
        int16_t left = 0;
        int16_t right = 0;
        capture_audio::ConvertFrame(source + i * format.blockAlign, format,
                                    left, right);
        const size_t destination =
            ((writeFrame_ + i) % capacityFrames_) * kChannels;
        data_[destination] = left;
        data_[destination + 1] = right;
    }
    writeFrame_ = (writeFrame_ + frames) % capacityFrames_;
    available_ += frames;
    PublishAvailable();
}

size_t PcmRing::Pop(int16_t* output, size_t frames) {
    if (!output || frames == 0) return 0;
    std::lock_guard<std::mutex> lock(mutex_);
    const size_t count = (std::min)(frames, available_);
    for (size_t i = 0; i < count; ++i) {
        const size_t source =
            ((readFrame_ + i) % capacityFrames_) * kChannels;
        const size_t destination = i * kChannels;
        output[destination] = data_[source];
        output[destination + 1] = data_[source + 1];
    }
    readFrame_ = (readFrame_ + count) % capacityFrames_;
    available_ -= count;
    PublishAvailable();
    return count;
}

size_t PcmRing::AvailableFrames() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return available_;
}

void PcmRing::Clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    readFrame_ = 0;
    writeFrame_ = 0;
    available_ = 0;
    PublishAvailable();
}

uint64_t PcmRing::Overruns() const noexcept {
    return overruns_.load(std::memory_order_relaxed);
}

void PcmRing::PublishAvailable() noexcept {
    if (publishedFrames_) {
        publishedFrames_->store(static_cast<UINT32>((std::min)(
            available_, static_cast<size_t>(UINT32_MAX))),
            std::memory_order_release);
    }
}

SincDriftResampler::SincDriftResampler(
    PcmRing& ring, std::atomic<UINT32>* publishedBufferedFrames) noexcept
    : ring_(ring), publishedBufferedFrames_(publishedBufferedFrames) {}

void SincDriftResampler::Prepare(size_t maxOutputFrames) {
    const size_t sourceFrames = (std::max)(
        static_cast<size_t>(32768), maxOutputFrames * 4 + 64);
    source_.reserve(sourceFrames * kChannels);
    transfer_.reserve((maxOutputFrames + kHalfTaps * 2 + 8) * kChannels);
}

void SincDriftResampler::Reset() {
    source_.clear();
    transfer_.clear();
    position_ = 0.0;
    primed_ = false;
    PublishBuffered();
}

size_t SincDriftResampler::Render(int16_t* output, size_t outputFrames,
                                  double ratio) {
    if (!output || outputFrames == 0) return 0;
    ratio = std::clamp(ratio, 0.999, 1.001);

    if (!primed_) {
        const size_t wanted = outputFrames + kHalfTaps * 2 + 2;
        const size_t pulled = AppendFromRing(wanted);
        if (pulled == 0) return 0;
        const int16_t firstLeft = source_[0];
        const int16_t firstRight = source_[1];
        source_.insert(source_.begin(), kHistoryFrames * kChannels, 0);
        for (int i = 0; i < kHistoryFrames; ++i) {
            source_[static_cast<size_t>(i) * kChannels] = firstLeft;
            source_[static_cast<size_t>(i) * kChannels + 1] = firstRight;
        }
        position_ = static_cast<double>(kHistoryFrames);
        primed_ = true;
    }

    const double lastPosition =
        position_ + ratio * static_cast<double>(outputFrames - 1);
    const size_t requiredFrames =
        static_cast<size_t>(std::floor(lastPosition)) + 1;
    const size_t currentFrames = source_.size() / kChannels;
    if (requiredFrames > currentFrames) {
        AppendFromRing(requiredFrames - currentFrames);
    }

    size_t produced = 0;
    const size_t sourceFrames = source_.size() / kChannels;
    while (produced < outputFrames) {
        const double samplePosition =
            position_ - static_cast<double>(kHalfTaps);
        const size_t center =
            static_cast<size_t>(std::floor(samplePosition));
        if (center < static_cast<size_t>(kHalfTaps - 1) ||
            center + kHalfTaps >= sourceFrames) {
            break;
        }
        const double fraction = samplePosition - static_cast<double>(center);
        for (size_t channel = 0; channel < kChannels; ++channel) {
            double sum = 0.0;
            double normalization = 0.0;
            for (int tap = -kHalfTaps + 1; tap <= kHalfTaps; ++tap) {
                const double distance = static_cast<double>(tap) - fraction;
                const double weight = WindowedSinc(distance);
                const size_t index =
                    (center + static_cast<size_t>(tap + kHalfTaps - 1) -
                     static_cast<size_t>(kHalfTaps - 1)) * kChannels + channel;
                sum += static_cast<double>(source_[index]) * weight;
                normalization += weight;
            }
            if (std::abs(normalization) > 1.0e-12) sum /= normalization;
            const long sample = std::lround(std::clamp(
                sum, static_cast<double>(INT16_MIN),
                static_cast<double>(INT16_MAX)));
            output[produced * kChannels + channel] =
                static_cast<int16_t>(sample);
        }
        ++produced;
        position_ += ratio;
    }

    CompactHistory();
    PublishBuffered();
    return produced;
}

size_t SincDriftResampler::BufferedFrames() const noexcept {
    const size_t frames = source_.size() / kChannels;
    const size_t consumed = static_cast<size_t>(std::floor(position_));
    return frames > consumed ? frames - consumed : 0;
}

double SincDriftResampler::Sinc(double value) noexcept {
    if (std::abs(value) < 1.0e-12) return 1.0;
    const double radians = kPi * value;
    return std::sin(radians) / radians;
}

double SincDriftResampler::WindowedSinc(double distance) noexcept {
    if (std::abs(distance) >= static_cast<double>(kHalfTaps)) return 0.0;
    return Sinc(distance) * Sinc(distance / static_cast<double>(kHalfTaps));
}

size_t SincDriftResampler::AppendFromRing(size_t wantedFrames) {
    const size_t frames = (std::min)(wantedFrames, ring_.AvailableFrames());
    if (frames == 0) return 0;
    transfer_.resize(frames * kChannels);
    const size_t pulled = ring_.Pop(transfer_.data(), frames);
    source_.insert(source_.end(), transfer_.begin(),
                   transfer_.begin() +
                       static_cast<ptrdiff_t>(pulled * kChannels));
    return pulled;
}

void SincDriftResampler::CompactHistory() {
    const size_t integerPosition =
        static_cast<size_t>(std::floor(position_));
    if (integerPosition <= static_cast<size_t>(kHistoryFrames)) return;
    const size_t dropFrames =
        integerPosition - static_cast<size_t>(kHistoryFrames);
    source_.erase(source_.begin(),
                  source_.begin() +
                      static_cast<ptrdiff_t>(dropFrames * kChannels));
    position_ -= static_cast<double>(dropFrames);
}

void SincDriftResampler::PublishBuffered() noexcept {
    if (publishedBufferedFrames_) {
        publishedBufferedFrames_->store(
            static_cast<UINT32>((std::min)(
                BufferedFrames(), static_cast<size_t>(UINT32_MAX))),
            std::memory_order_release);
    }
}

}  // namespace llcv::audio
