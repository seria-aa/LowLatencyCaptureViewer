#pragma once

#include <algorithm>
#include <cstdint>

namespace llcv::diagnostics {

// UI/diagnostic-thread observer, not a capture throttle. It consumes the
// existing cumulative input counter and never sleeps, queues or drops frames.
// Driver-reported media-type FPS alone cannot prove the actual delivery rate.
class CaptureFrameRateMonitor {
public:
    enum class Event { None, Overdelivery, Recovered };
    struct Observation {
        Event event = Event::None;
        double measuredFps = 0.0;
        bool overdelivery = false;
        bool rateMeasured = false;
    };

    Observation Observe(uint64_t nowMilliseconds, uint64_t cumulativeFrames,
                        int requestedFps) noexcept {
        if (requestedFps <= 0) {
            Reset(); // Automatic mode promises no explicit input rate.
            return {};
        }
        if (!initialized_ || requestedFps != requestedFps_ ||
            nowMilliseconds < lastMilliseconds_ ||
            cumulativeFrames < lastFrames_ ||
            nowMilliseconds - lastMilliseconds_ > kMaximumObservationGapMs) {
            Start(nowMilliseconds, cumulativeFrames, requestedFps);
            return {};
        }
        const uint64_t elapsed = nowMilliseconds - lastMilliseconds_;
        // Ignore duplicate UI updates and accumulate sub-window observations.
        if (elapsed < kMinimumObservationMs)
            return {Event::None, 0.0, overdelivery_, false};

        const uint64_t frames = cumulativeFrames - lastFrames_;
        const uint64_t observationStart = lastMilliseconds_;
        lastMilliseconds_ = nowMilliseconds;
        lastFrames_ = cumulativeFrames;
        if (!frames) {
            // No flow is neither evidence of excess FPS nor of recovery.
            // A new flow gets a fresh grace period instead of inheriting a
            // previous stream's pending/active warning.
            Start(nowMilliseconds, cumulativeFrames, requestedFps);
            return {};
        }
        const double measured = static_cast<double>(frames) * 1000.0 /
                                static_cast<double>(elapsed);
        Observation result{Event::None, measured, overdelivery_, true};
        // An interval spanning the grace boundary can include a startup burst;
        // do not count its whole duration as post-grace evidence.
        if (nowMilliseconds - startedMilliseconds_ <= kStartupGraceMs ||
            observationStart - startedMilliseconds_ < kStartupGraceMs) return result;

        const double excessThreshold = requestedFps +
            (std::max)(2.0, static_cast<double>(requestedFps) * 0.10);
        const double recoveryThreshold = requestedFps +
            (std::max)(1.0, static_cast<double>(requestedFps) * 0.05);
        if (measured > excessThreshold) {
            recoveryMilliseconds_ = 0;
            if (!overdelivery_) {
                excessMilliseconds_ += elapsed;
                if (excessMilliseconds_ >= kConfirmationMs) {
                    overdelivery_ = true;
                    result.event = Event::Overdelivery;
                }
            }
        } else {
            excessMilliseconds_ = 0;
            if (overdelivery_ && measured <= recoveryThreshold) {
                recoveryMilliseconds_ += elapsed;
                if (recoveryMilliseconds_ >= kConfirmationMs) {
                    overdelivery_ = false;
                    recoveryMilliseconds_ = 0;
                    result.event = Event::Recovered;
                }
            } else {
                recoveryMilliseconds_ = 0;
            }
        }
        result.overdelivery = overdelivery_;
        return result;
    }

    void Reset() noexcept {
        initialized_ = false;
        overdelivery_ = false;
        requestedFps_ = 0;
        lastMilliseconds_ = lastFrames_ = startedMilliseconds_ = 0;
        excessMilliseconds_ = recoveryMilliseconds_ = 0;
    }

private:
    void Start(uint64_t nowMilliseconds, uint64_t cumulativeFrames,
               int requestedFps) noexcept {
        Reset();
        initialized_ = true;
        requestedFps_ = requestedFps;
        startedMilliseconds_ = lastMilliseconds_ = nowMilliseconds;
        lastFrames_ = cumulativeFrames;
    }

    static constexpr uint64_t kMinimumObservationMs = 250;
    static constexpr uint64_t kMaximumObservationGapMs = 2500;
    static constexpr uint64_t kStartupGraceMs = 3000;
    static constexpr uint64_t kConfirmationMs = 3000;
    bool initialized_ = false;
    bool overdelivery_ = false;
    int requestedFps_ = 0;
    uint64_t startedMilliseconds_ = 0;
    uint64_t lastMilliseconds_ = 0;
    uint64_t lastFrames_ = 0;
    uint64_t excessMilliseconds_ = 0;
    uint64_t recoveryMilliseconds_ = 0;
};

}  // namespace llcv::diagnostics
