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

double PeakToDbfs(int sample) noexcept {
    if (sample <= 0) return -96.0;
    return 20.0 * std::log10(sample / 32767.0);
}

}  // namespace llcv::audio
