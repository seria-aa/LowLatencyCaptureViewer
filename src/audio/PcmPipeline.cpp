#include "audio/PcmPipeline.h"

#include <algorithm>
#include <cmath>
#include <climits>
#include <limits>
#include <stdexcept>

namespace llcv::audio {

PcmRing::PcmRing(size_t capacityFrames,
                 std::atomic<UINT32>* publishedFrames,
                 OverrunObserver overrunObserver, void* observerContext)
    : capacityFrames_((std::max)(size_t{1}, capacityFrames)),
      data_(capacityFrames_ * channels_),
      publishedFrames_(publishedFrames),
      overrunObserver_(overrunObserver),
      observerContext_(observerContext) {
    PublishAvailable();
}

void PcmRing::ConfigureChannels(size_t channels) {
    if (channels != 2 && channels != 6) throw std::invalid_argument("PCM channels");
    std::lock_guard<std::mutex> lock(mutex_);
    data_.assign(capacityFrames_ * channels, 0);
    channels_ = channels;
    readFrame_ = writeFrame_ = available_ = 0;
    PublishAvailable();
}

size_t PcmRing::PrepareWrite(size_t frames) {
    if (frames >= capacityFrames_) {
        const size_t dropped = available_ + (frames - capacityFrames_);
        if (dropped && overrunObserver_ &&
            overrunObserver_(observerContext_, dropped)) {
            overruns_.fetch_add(1, std::memory_order_relaxed);
        }
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
        samples += (frames - capacityFrames_) * channels_;
    }
    frames = PrepareWrite(frames);
    for (size_t i = 0; i < frames; ++i) {
        const size_t destination =
            ((writeFrame_ + i) % capacityFrames_) * channels_;
        const size_t source = i * channels_;
        if (channels_ == 2) {
            data_[destination] = samples[source];
            data_[destination + 1] = samples[source + 1];
        } else std::copy_n(samples + source, channels_, data_.data() + destination);
    }
    writeFrame_ = (writeFrame_ + frames) % capacityFrames_;
    available_ += frames;
    PublishAvailable();
}

void PcmRing::PushConverted(const BYTE* source, size_t frames,
                            const capture_audio::Format& format) {
    if (!source || frames == 0 || format.blockAlign == 0) return;
    if ((channels_ == 6) != (format.path == capture_audio::Path::ConvertToSurround51)) return;
    std::lock_guard<std::mutex> lock(mutex_);
    if (frames >= capacityFrames_) {
        source += (frames - capacityFrames_) * format.blockAlign;
    }
    frames = PrepareWrite(frames);
    for (size_t i = 0; i < frames; ++i) {
        const size_t destination =
            ((writeFrame_ + i) % capacityFrames_) * channels_;
        if (channels_ == 6) {
            capture_audio::ConvertSurroundFrame(source + i * format.blockAlign,
                format, data_.data() + destination);
        } else {
            capture_audio::ConvertFrame(source + i * format.blockAlign, format,
                data_[destination], data_[destination + 1]);
        }
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
            ((readFrame_ + i) % capacityFrames_) * channels_;
        const size_t destination = i * channels_;
        if (channels_ == 2) {
            output[destination] = data_[source];
            output[destination + 1] = data_[source + 1];
        } else std::copy_n(data_.data() + source, channels_, output + destination);
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
    : channels_(ring.Channels()), ring_(ring), publishedBufferedFrames_(publishedBufferedFrames) {}

void SincDriftResampler::Prepare(size_t maxOutputFrames) {
    const size_t sourceFrames = (std::max)(
        static_cast<size_t>(32768), maxOutputFrames * 4 + 64);
    source_.reserve(sourceFrames * channels_);
    transfer_.reserve((maxOutputFrames + kHalfTaps * 2 + 8) * channels_);
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
        int16_t first[6]{};
        std::copy_n(source_.data(), channels_, first);
        source_.insert(source_.begin(), kHistoryFrames * channels_, 0);
        for (int i = 0; i < kHistoryFrames; ++i) {
            std::copy_n(first, channels_, source_.data() + static_cast<size_t>(i) * channels_);
        }
        position_ = static_cast<double>(kHistoryFrames);
        primed_ = true;
    }

    const double lastPosition =
        position_ + ratio * static_cast<double>(outputFrames - 1);
    const size_t requiredFrames =
        static_cast<size_t>(std::floor(lastPosition)) + 1;
    const size_t currentFrames = source_.size() / channels_;
    if (requiredFrames > currentFrames) {
        AppendFromRing(requiredFrames - currentFrames);
    }

    size_t produced = 0;
    const size_t sourceFrames = source_.size() / channels_;
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
        double weights[kHalfTaps * 2];
        for (int tap = -kHalfTaps + 1; tap <= kHalfTaps; ++tap) {
            weights[tap + kHalfTaps - 1] =
                WindowedSinc(static_cast<double>(tap) - fraction);
        }
        for (size_t channel = 0; channel < channels_; ++channel) {
            double sum = 0.0;
            double normalization = 0.0;
            for (int tap = -kHalfTaps + 1; tap <= kHalfTaps; ++tap) {
                const double weight = weights[tap + kHalfTaps - 1];
                const size_t index =
                    (center + static_cast<size_t>(tap + kHalfTaps - 1) -
                     static_cast<size_t>(kHalfTaps - 1)) * channels_ + channel;
                sum += static_cast<double>(source_[index]) * weight;
                normalization += weight;
            }
            if (std::abs(normalization) > 1.0e-12) sum /= normalization;
            const long sample = std::lround(std::clamp(
                sum, static_cast<double>(INT16_MIN),
                static_cast<double>(INT16_MAX)));
            output[produced * channels_ + channel] =
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
    const size_t frames = source_.size() / channels_;
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
    transfer_.resize(frames * channels_);
    const size_t pulled = ring_.Pop(transfer_.data(), frames);
    source_.insert(source_.end(), transfer_.begin(),
                   transfer_.begin() +
                       static_cast<ptrdiff_t>(pulled * channels_));
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
                      static_cast<ptrdiff_t>(dropFrames * channels_));
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
