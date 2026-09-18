#include "audio/AudioMix.h"

#include <algorithm>
#include <climits>
#include <cmath>

namespace llcv::audio {

MixMetrics ProcessStereoPcm(int16_t* samples, std::size_t frames,
                            StereoGain& current, StereoGain target,
                            bool measurePeaks) noexcept {
    MixMetrics result{};
    if (!samples || frames == 0) {
        current = target;
        return result;
    }

    const bool process = current.left != 1.0 || current.right != 1.0 ||
        target.left != 1.0 || target.right != 1.0;
    if (!process && !measurePeaks) return result;

    const double leftStep = (target.left - current.left) /
        static_cast<double>(frames);
    const double rightStep = (target.right - current.right) /
        static_cast<double>(frames);
    double leftGain = current.left;
    double rightGain = current.right;
    for (std::size_t frame = 0; frame < frames; ++frame) {
        leftGain += leftStep;
        rightGain += rightStep;
        const std::size_t index = frame * 2;
        int16_t left = samples[index];
        int16_t right = samples[index + 1];
        if (process) {
            const long scaledLeft = std::lround(
                static_cast<double>(left) * leftGain);
            const long scaledRight = std::lround(
                static_cast<double>(right) * rightGain);
            result.clipped = result.clipped || scaledLeft < INT16_MIN ||
                scaledLeft > INT16_MAX || scaledRight < INT16_MIN ||
                scaledRight > INT16_MAX;
            left = static_cast<int16_t>(std::clamp(
                scaledLeft, static_cast<long>(INT16_MIN),
                static_cast<long>(INT16_MAX)));
            right = static_cast<int16_t>(std::clamp(
                scaledRight, static_cast<long>(INT16_MIN),
                static_cast<long>(INT16_MAX)));
            samples[index] = left;
            samples[index + 1] = right;
        }
        if (measurePeaks) {
            result.peakLeft = (std::max)(result.peakLeft,
                std::abs(static_cast<int>(left)));
            result.peakRight = (std::max)(result.peakRight,
                std::abs(static_cast<int>(right)));
        }
    }
    current = target;
    return result;
}

int DecayAndHoldPeak(int previous, int observed) noexcept {
    const int decayed = previous > 0 ? previous - (std::max)(1, previous / 20)
                                     : 0;
    return (std::max)(observed, decayed);
}

MixMetrics ProcessSurroundPcm(int16_t* samples, std::size_t frames,
    std::array<double, 6>& current, double master, StereoGain sides,
    bool measurePeaks) noexcept {
    MixMetrics result{};
    const std::array<double, 6> target{master * sides.left, master * sides.right,
        master, master, master * sides.left, master * sides.right};
    if (!samples || !frames) { current = target; return result; }
    bool process = false;
    std::array<double, 6> step{};
    for (size_t c = 0; c < 6; ++c) {
        process |= current[c] != 1.0 || target[c] != 1.0;
        step[c] = (target[c] - current[c]) / static_cast<double>(frames);
    }
    if (!process && !measurePeaks) return result;
    for (size_t f = 0; f < frames; ++f) {
        for (size_t c = 0; c < 6; ++c) {
            current[c] += step[c];
            int16_t& sample = samples[f * 6 + c];
            if (process) {
                const long value = std::lround(sample * current[c]);
                result.clipped |= value < INT16_MIN || value > INT16_MAX;
                sample = static_cast<int16_t>(std::clamp(value,
                    static_cast<long>(INT16_MIN), static_cast<long>(INT16_MAX)));
            }
            if (measurePeaks) {
                const int peak = std::abs(static_cast<int>(sample));
                if (c != 1 && c != 5) result.peakLeft = (std::max)(result.peakLeft, peak);
                if (c != 0 && c != 4) result.peakRight = (std::max)(result.peakRight, peak);
            }
        }
    }
    current = target;
    return result;
}

double PeakToDbfs(int sample) noexcept {
    if (sample <= 0) return -96.0;
    return 20.0 * std::log10(sample / 32767.0);
}

}  // namespace llcv::audio
