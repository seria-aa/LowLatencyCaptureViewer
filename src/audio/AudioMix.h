#pragma once

#include <cstddef>
#include <cstdint>
#include <array>

namespace llcv::audio {

struct StereoGain {
    double left = 1.0;
    double right = 1.0;
};

struct MixMetrics {
    int peakLeft = 0;
    int peakRight = 0;
    bool clipped = false;
};

// Applies a click-free linear gain ramp in place. With both gains at 1.0 and
// metering disabled, this returns without touching the PCM buffer.
MixMetrics ProcessStereoPcm(int16_t* samples, std::size_t frames,
                            StereoGain& current, StereoGain target,
                            bool measurePeaks) noexcept;
// Master affects all six channels. L/R controls affect the corresponding
// front/surround pair; center and LFE use master only. Meters summarize sides.
MixMetrics ProcessSurroundPcm(int16_t* samples, std::size_t frames,
    std::array<double, 6>& current, double master, StereoGain sides,
    bool measurePeaks) noexcept;

// Peak hold is intentionally a tiny arithmetic helper, not a second audio
// buffer. The caller owns the atomic publication policy.
int DecayAndHoldPeak(int previous, int observed) noexcept;
double PeakToDbfs(int sample) noexcept;

}  // namespace llcv::audio
