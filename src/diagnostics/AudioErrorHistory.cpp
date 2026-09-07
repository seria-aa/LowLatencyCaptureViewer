#include "diagnostics/AudioErrorHistory.h"

#include <algorithm>

namespace llcv::diagnostics {

AudioErrorHistory::AudioErrorHistory(size_t capacity) noexcept
    : capacity_((std::max)(size_t{1}, capacity)) {}

void AudioErrorHistory::Record(const AudioErrorEvent& event) {
    if (event.timestampMs == 0) return;
    std::lock_guard<std::mutex> lock(mutex_);
    if (events_.size() >= capacity_) events_.pop_front();
    events_.push_back(event);
}

AudioPatternStats AudioErrorHistory::Analyze(
    uint64_t nowMs, uint32_t packetFrames, uint32_t sampleRate,
    uint64_t windowMs) const {
    const uint64_t windowStartMs = nowMs > windowMs ? nowMs - windowMs : 0;
    const uint64_t packetPeriodMs = packetFrames && sampleRate
        ? (1000ull * packetFrames + sampleRate - 1) / sampleRate
        : 10;
    // Two packet periods apart still belong to the same short burst. This is
    // display classification only; it does not change underrun accounting.
    const uint64_t burstGapMs = (std::max)(20ull, packetPeriodMs * 2);

    AudioPatternStats stats{};
    size_t currentBurst = 0;
    uint64_t previousUnderrunMs = 0;
    std::lock_guard<std::mutex> lock(mutex_);
    for (const AudioErrorEvent& event : events_) {
        if (event.timestampMs < windowStartMs) continue;
        ++stats.recentEvents;
        stats.lastEventMs = (std::max)(stats.lastEventMs, event.timestampMs);
        if (event.kind != AudioErrorKind::Underrun) {
            currentBurst = 0;
            previousUnderrunMs = 0;
            continue;
        }
        ++stats.recentUnderruns;
        if (previousUnderrunMs != 0 &&
            event.timestampMs >= previousUnderrunMs &&
            event.timestampMs - previousUnderrunMs <= burstGapMs) {
            ++currentBurst;
        } else {
            currentBurst = 1;
        }
        stats.maxConsecutiveUnderruns =
            (std::max)(stats.maxConsecutiveUnderruns, currentBurst);
        previousUnderrunMs = event.timestampMs;
    }
    return stats;
}

size_t AudioErrorHistory::Size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return events_.size();
}

}  // namespace llcv::diagnostics
