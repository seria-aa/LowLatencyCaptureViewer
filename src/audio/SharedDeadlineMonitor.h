#pragma once

#include <cstdint>

namespace llcv::audio {
// Evidence of output queue exhaustion, NOT a measurement of audible loss.
// Padding is sampled and driver reporting can lag; allow 2 ms tolerance.
class SharedDeadlineMonitor {
public:
    bool Observe(double seconds, uint32_t padding) {
        const bool suspect = armed_ && padding == 0 &&
            seconds > previousSeconds_ + queued_ / 48000.0 + 0.002;
        if (suspect) overdue_ = seconds - previousSeconds_ - queued_ / 48000.0;
        previousSeconds_ = seconds;
        queued_ = padding;
        armed_ = true;
        reported_ = suspect;
        return suspect;
    }
    bool Submitted(double seconds, uint32_t written) {
        const bool suspect = armed_ && !reported_ &&
            seconds > previousSeconds_ + queued_ / 48000.0 + 0.002;
        if (suspect) overdue_ = seconds - previousSeconds_ - queued_ / 48000.0;
        const double consumed = (seconds - previousSeconds_) * 48000.0;
        queued_ = (consumed >= queued_ ? 0u :
            static_cast<uint32_t>(queued_ - consumed)) + written;
        previousSeconds_ = seconds;
        return suspect;
    }
    double OverdueSeconds() const { return overdue_; }
private:
    double overdue_ = 0;
    double previousSeconds_ = 0;
    uint32_t queued_ = 0;
    bool armed_ = false;
    bool reported_ = false;
};
} // namespace llcv::audio
