#pragma once

#include <algorithm>
#include <cstddef>

namespace llcv::audio {
// Proportional queue regulation with an integral shortage bias. Negative
// drift must not permanently consume reserve. Positive drift retains the
// original P controller's extra headroom: removing it regressed packet-jitter
// tolerance. The bias can unwind to zero but never erodes that useful margin.
class QueueDriftController {
public:
    // Auto can engage with only one output block left. Start conservatively
    // in that case rather than spending the remaining reserve ramping from 0.
    void BeginWithLowReserve() { ppm_ = -1000.0; }
    double Update(double filteredFrames, double targetFrames, size_t frames) {
        const double seconds = (std::min)(frames / 48000.0, 0.05);
        // Retain one 16-tap resampler window (0.33 ms), including fractional
        // source demand. This is bounded DSP headroom, not an adaptive target.
        const double error = filteredFrames - (targetFrames + 16.0);
        const double proportional = error * 2.0;
        const double request = proportional + clockPpm_;
        // Conditional integration prevents windup during stalls / bursts.
        if ((request < 1000.0 || error < 0.0) &&
            (request > -1000.0 || error > 0.0)) {
            clockPpm_ = std::clamp(clockPpm_ + error * seconds * 0.1,
                                   -1000.0, 0.0);
        }
        const double desired = std::clamp(proportional + clockPpm_, -1000.0, 1000.0);
        ppm_ += (desired - ppm_) * seconds / (0.5 + seconds);
        return ppm_;
    }
private:
    double clockPpm_ = 0.0;
    double ppm_ = 0.0;
};
} // namespace llcv::audio
