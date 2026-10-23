#include "diagnostics/CaptureFrameRateMonitor.h"

#include <cstdio>
#include <limits>

using Monitor = llcv::diagnostics::CaptureFrameRateMonitor;
using Event = Monitor::Event;

static unsigned failures = 0;
static void Check(bool condition, const char* description) {
    if (!condition) {
        std::fprintf(stderr, "FAILED: %s\n", description);
        ++failures;
    }
}

struct Feed {
    Monitor monitor;
    uint64_t time = 0;
    uint64_t frames = 0;
    int requested = 60;

    Feed(int fps = 60) : requested(fps) { monitor.Observe(time, frames, requested); }
    Monitor::Observation Add(uint64_t duration, uint64_t count) {
        time += duration;
        frames += count;
        return monitor.Observe(time, frames, requested);
    }
};

static void TestRates() {
    for (int fps : {24, 25, 30, 40, 50, 60, 90, 100, 120, 144, 240}) {
        Feed feed(fps);
        for (unsigned i = 0; i < 120; ++i) {
            const auto result = feed.Add(1000, fps);
            Check(result.rateMeasured && result.event == Event::None && !result.overdelivery,
                  "ordinary selected FPS is not flagged");
        }
    }
    for (int fps : {30, 60}) {
        Feed ntsc(fps);
        for (unsigned i = 0; i < 100; ++i) {
            const auto result = ntsc.Add(1001, fps);
            Check(result.event == Event::None && !result.overdelivery,
                  "29.97/59.94 delivery is not flagged as a nominal FPS mismatch");
        }
        Feed excessive(fps);
        unsigned warnings = 0;
        for (unsigned second = 1; second <= 20; ++second) {
            const auto result = excessive.Add(1000, 120);
            warnings += result.event == Event::Overdelivery;
            Check(second < 6 ? !result.overdelivery : result.overdelivery,
                  "sustained overdelivery needs startup grace and three measured seconds");
        }
        Check(warnings == 1, "persistent excess FPS emits exactly one transition, not log spam");
        for (unsigned second = 1; second <= 5; ++second) {
            const auto result = excessive.Add(1000, fps);
            Check((result.event == Event::Recovered) == (second == 3),
                  "recovery requires sustained valid delivery and emits once");
            Check(second < 3 ? result.overdelivery : !result.overdelivery,
                  "recovery clears the active warning");
        }
        for (unsigned second = 1; second <= 3; ++second) {
            const auto result = excessive.Add(1000, 120);
            Check((result.event == Event::Overdelivery) == (second == 3),
                  "later genuine mismatch can raise a fresh warning");
        }
    }
}

static void TestJitterAndThresholds() {
    Feed jitter;
    for (unsigned i = 0; i < 80; ++i) {
        // A delayed UI observation consumes the corresponding elapsed time.
        const auto result = jitter.Add(i % 2 ? 1200 : 800, i % 2 ? 72 : 48);
        Check(!result.overdelivery && result.event == Event::None,
              "UI scheduling jitter does not change measured input FPS");
    }
    Feed burst;
    for (unsigned i = 0; i < 8; ++i) burst.Add(1000, 60);
    for (unsigned i = 0; i < 20; ++i) {
        const auto result = burst.Add(1000, i % 3 < 2 ? 90 : 1);
        Check(!result.overdelivery, "short driver delivery bursts do not count as sustained excess FPS");
    }
    Feed edge;
    for (unsigned i = 0; i < 12; ++i)
        Check(!edge.Add(1000, 66).overdelivery, "10 percent threshold boundary is tolerated");
    for (unsigned i = 0; i < 3; ++i) edge.Add(1000, 120);
    for (unsigned i = 0; i < 8; ++i) {
        const auto result = edge.Add(1000, 64);
        Check(result.overdelivery && result.event == Event::None,
              "hysteresis avoids warning/recovery oscillation near threshold");
    }
    Feed partialGrace;
    partialGrace.Add(2500, 300);
    partialGrace.Add(2500, 300);
    Check(!partialGrace.Add(500, 60).overdelivery,
          "interval crossing startup grace cannot count its pre-grace duration as evidence");
    Check(!partialGrace.Add(2000, 240).overdelivery,
          "only wholly post-grace observations count toward sustained excess");
    Check(partialGrace.Add(500, 60).overdelivery,
          "irregular post-grace observations eventually confirm sustained excess");
}

static void TestResetAndGaps() {
    Feed feed;
    for (unsigned i = 0; i < 6; ++i) feed.Add(1000, 120);
    auto result = feed.Add(1000, 0);
    Check(!result.overdelivery && result.event == Event::None && !result.rateMeasured,
          "no input clears stale state without claiming measured recovery");
    for (unsigned i = 0; i < 5; ++i)
        Check(!feed.Add(1000, 120).overdelivery, "new flow gets a complete grace period");
    Check(feed.Add(1000, 120).overdelivery, "new sustained excess flow is eventually detected");
    result = feed.Add(10000, 1200);
    Check(!result.rateMeasured && !result.overdelivery && result.event == Event::None,
          "long UI stalls reset rather than classifying stale accumulated counters");

    feed.requested = 120;
    Check(!feed.Add(1000, 120).rateMeasured, "target changes restart rate observation");
    for (unsigned i = 0; i < 12; ++i)
        Check(!feed.Add(1000, 120).overdelivery, "new target uses its own rate threshold");
    feed.requested = 0;
    for (unsigned i = 0; i < 12; ++i)
        Check(!feed.Add(1000, 240).rateMeasured, "automatic mode does not impose a hidden FPS promise");
    feed.requested = 60;
    feed.monitor.Observe(feed.time, feed.frames, feed.requested);
    result = feed.monitor.Observe(feed.time - 1, feed.frames, feed.requested);
    Check(!result.rateMeasured && !result.overdelivery, "backwards clock resets without unsigned underflow");
    result = feed.monitor.Observe(feed.time, 0, feed.requested);
    Check(!result.rateMeasured && !result.overdelivery, "reset counters cannot become enormous FPS");

    Feed quick;
    for (unsigned i = 0; i < 800; ++i) {
        const auto sample = quick.Add(25, i % 2 ? 2 : 1);
        Check(!sample.overdelivery, "short observations accumulate safely instead of dividing by near zero");
    }
    result = quick.monitor.Observe(quick.time, quick.frames, 60);
    Check(!result.rateMeasured, "duplicate time is not a fresh rate observation");

    Monitor large;
    const auto maximum = (std::numeric_limits<uint64_t>::max)();
    large.Observe(100, maximum - 500, 60);
    result = large.Observe(1100, maximum - 440, 60);
    Check(result.rateMeasured && result.measuredFps == 60.0,
          "large cumulative counter subtracts before floating point conversion");
    result = large.Observe(2100, 10, 60);
    Check(!result.rateMeasured, "wrapped counter resets without an excess-rate report");
}

int main() {
    TestRates();
    TestJitterAndThresholds();
    TestResetAndGaps();
    std::printf("Capture FPS monitor: %u failures; deterministic counters only, no device or pacing changes.\n",
                failures);
    return failures ? 1 : 0;
}
