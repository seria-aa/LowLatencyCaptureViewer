// Test-only extension of SharedReplayTests. Uses the real FillWasapiPcm;
// source clock, FIFO delivery jitter and endpoint consumption are independent.
#include <deque>

enum class ClockProfile { Fixed, FixedZero, Reverse300, Reverse800, Ramp300 };
struct ExtendedCase {
    const char* name;
    int targetMs = 20;
    int periodFrames = 480;
    ClockProfile clock = ClockProfile::Fixed;
    uint32_t seed = 1;
    int jitterMs = 0;
    int inputPauseMs = 0;
    bool discard = false;
    int outputPauseMs = 0;
    DriftCorrectionMode mode = DriftCorrectionMode::Auto;
};

static double ClockPpm(ClockProfile profile, double timeMs) {
    switch (profile) {
    case ClockProfile::FixedZero: return 0;
    case ClockProfile::Reverse300: return std::fmod(timeMs, 120000.0) < 60000 ? 300 : -300;
    case ClockProfile::Reverse800: return std::fmod(timeMs, 120000.0) < 60000 ? 800 : -800;
    case ClockProfile::Ramp300: {
        const double phase = std::fmod(timeMs, 240000.0) / 120000.0;
        return phase < 1 ? -300 + 600 * phase : 900 - 600 * phase;
    }
    default: return -100;
    }
}
// Deliberately stable PRNG: a failed seed is exactly reproducible across runs.
static uint32_t RandomStep(uint32_t& seed) {
    seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
    return seed;
}
static bool InPause(double timeMs, int durationMs, int seconds) {
    if (!durationMs || timeMs < 60000) return false;
    const double start = 60000 + std::floor((timeMs - 60000) / 120000) * 120000;
    return start <= (seconds - 60) * 1000.0 && timeMs < start + durationMs;
}
static bool NearPause(double timeMs, int durationMs, int seconds) {
    return InPause(timeMs, durationMs ? durationMs + 2000 : 0, seconds);
}

using ReplayFill = llcv::wasapi::FillResult (*)(void*, int16_t*, size_t);
static void RunExtended(const ExtendedCase& test, int seconds,
                        ReplayFill fill = FillWasapiPcm, bool legacyReference = false) {
    std::printf("START %s target=%d period=%.3f seed=%u duration=%d pause=%d discard=%d reference=%d\n",
        test.name, test.targetMs, test.periodFrames / 48.0, test.seed, seconds,
        test.inputPauseMs, test.discard, legacyReference);
    std::fflush(stdout);
    const unsigned failuresBefore = failures;
    g_running.store(true);
    g_settings.driftCorrection = test.mode;
    g_audioTrackingStartMs.store(6000);
    g_audioCapturePacketFrames.store(480);
    g_audioMinimumPreRenderFrames.store(UINT32_MAX);
    g_underruns.store(0);
    g_audioUnderrunFrames.store(0);
    g_audioResamplerUnderruns.store(0);
    g_audioLatePacketUnderruns.store(0);
    g_sharedRebuffers.store(0);
    g_sharedRebufferSilenceFrames.store(0);
    g_sharedRebuffering.store(false);
    g_ring.Clear();
    g_audioResamplerFrames.store(0);
    WasapiRenderState state;
    state.queueTargetFrames = test.targetMs * 48;
    state.autoCorrectionActive = test.mode == DriftCorrectionMode::Resample;
    const size_t capacity = test.periodFrames * 2 + 96;
    state.driftResampler.Prepare(capacity);
    const auto oldOverruns = g_ring.Overruns();
    std::vector<int16_t> packet(960, 1000), output(capacity * 2 + 2, -23456);
    std::deque<double> deliveries;
    uint32_t seed = test.seed;
    double nextSourceMs = 2, lastDeliveryMs = 0;
    const double periodMs = test.periodFrames / 48.0;
    size_t padding = capacity;
    uint64_t renderedMissing = 0, outsideFaultMissing = 0, endpointMissing = 0;
    uint64_t maxBurstMissing = 0, burstMissing = 0;
    uint64_t maxQueue = 0, maxSettledQueue = 0;
    double firstLoss = -1, lastLoss = -1;
    double rebufferStart = -1, maxRebufferWait = 0;
    unsigned deadlines = 0, autoTransitions = 0;
    bool previousAuto = state.autoCorrectionActive;
    llcv::audio::SharedDeadlineMonitor monitor;
    for (uint64_t step = 1; step * periodMs <= seconds * 1000.0; ++step) {
        const double timeMs = step * periodMs;
        replayMilliseconds = 1000 + static_cast<uint64_t>(timeMs);
        if (timeMs > 5000 && padding < static_cast<size_t>(test.periodFrames))
            endpointMissing += test.periodFrames - padding;
        padding = padding > static_cast<size_t>(test.periodFrames) ? padding - test.periodFrames : 0;
        // Generate by the capture oscillator, independently of output wakeups.
        while (nextSourceMs <= timeMs) {
            const double jitter = test.jitterMs * (RandomStep(seed) / 4294967295.0);
            const double due = (std::max)(lastDeliveryMs, nextSourceMs + jitter);
            if (!(test.discard && InPause(nextSourceMs, test.inputPauseMs, seconds)))
                deliveries.push_back(due);
            lastDeliveryMs = due;
            nextSourceMs += 10.0 / (1.0 + ClockPpm(test.clock, nextSourceMs) / 1e6);
        }
        if (!InPause(timeMs, test.inputPauseMs, seconds)) {
            while (!deliveries.empty() && deliveries.front() <= timeMs) {
                g_ring.Push(packet.data(), 480);
                g_audioLastCaptureCallbackMs.store(replayMilliseconds);
                deliveries.pop_front();
            }
        }
        if (InPause(timeMs, test.outputPauseMs, seconds)) continue;
        const size_t writable = capacity - padding;
        if (monitor.Observe(timeMs / 1000, static_cast<UINT32>(padding)) && timeMs > 5000)
            ++deadlines;
        const auto result = fill(&state, output.data(), writable);
        monitor.Submitted(timeMs / 1000, static_cast<UINT32>(writable));
        padding += writable;
        Check(output[capacity * 2] == -23456 && output[capacity * 2 + 1] == -23456,
              "extended: callback must not write beyond output allocation");
        Check(result.writtenFrames <= writable && std::isfinite(state.correctionPpm) &&
              std::abs(state.correctionPpm) <= 1000.01,
              "extended: finite bounded correction and valid output length");
        Check(state.queueTargetFrames == static_cast<UINT32>(test.targetMs * 48),
              "extended: recovery must never increase the configured target");
        if (previousAuto != state.autoCorrectionActive) ++autoTransitions;
        previousAuto = state.autoCorrectionActive;
        if (state.rebuffering && rebufferStart < 0) rebufferStart = timeMs;
        if (!state.rebuffering && rebufferStart >= 0) {
            maxRebufferWait = (std::max)(maxRebufferWait, timeMs - rebufferStart);
            rebufferStart = -1;
        }
        if (timeMs <= 5000 || !state.audioStarted) continue;
        maxQueue = (std::max)(maxQueue, static_cast<uint64_t>(result.availableBeforeRender));
        if (!NearPause(timeMs, test.inputPauseMs, seconds) &&
            !NearPause(timeMs, test.outputPauseMs, seconds))
            maxSettledQueue = (std::max)(maxSettledQueue, static_cast<uint64_t>(result.availableBeforeRender));
        const uint64_t missing = writable - result.writtenFrames;
        renderedMissing += missing;
        if (missing) {
            if (firstLoss < 0) firstLoss = timeMs;
            lastLoss = timeMs;
            burstMissing += missing;
            maxBurstMissing = (std::max)(maxBurstMissing, burstMissing);
            if (!NearPause(timeMs, test.inputPauseMs, seconds) &&
                !NearPause(timeMs, test.outputPauseMs, seconds)) outsideFaultMissing += missing;
        } else burstMissing = 0;
    }
    // Invariants are enforced before looking at results; fault cases are not
    // falsely called lossless, and normal conditions must not invent output faults.
    Check(renderedMissing == g_audioUnderrunFrames.load() + g_sharedRebufferSilenceFrames.load(),
          "extended: every missing frame must be accounted for, including intentional silence");
    Check(g_ring.Overruns() == oldOverruns, "extended: no queue overflow");
    Check(autoTransitions <= 1, "extended: Auto must latch, not oscillate on/off");
    Check(!state.rebuffering && rebufferStart < 0, "extended: recovery must finish");
    Check(maxRebufferWait <= test.inputPauseMs + test.targetMs + 2 * periodMs,
          "extended: recovery wait must remain bounded by missing input and reserve");
    Check(maxQueue <= static_cast<uint64_t>((test.targetMs + test.inputPauseMs +
          test.outputPauseMs + test.jitterMs + 40) * 48), "extended: queue must remain bounded");
    if (test.outputPauseMs) Check(deadlines > 0 && endpointMissing > 0,
          "extended: repeated output stalls must have independent deadline evidence");
    else Check(deadlines == 0 && endpointMissing == 0,
          "extended: normal endpoint pacing must not be diagnosed as a scheduling fault");
    if (!legacyReference && test.inputPauseMs && !test.jitterMs && test.clock == ClockProfile::FixedZero) {
        Check(outsideFaultMissing == 0,
              "extended: injected input loss must not cause recurring losses after recovery");
        Check(maxBurstMissing <= static_cast<uint64_t>((test.inputPauseMs + test.targetMs + 2 * periodMs) * 48),
              "extended: reprime must not extend one interruption without bound");
        const uint64_t injected = seconds >= 120 ? 1 + (seconds - 120) / 120 : 0;
        Check(g_sharedRebuffers.load() <= injected,
              "extended: one input interruption must not cause multiple reprimes");
        if (test.mode != DriftCorrectionMode::Off)
            Check(state.filteredQueuedFrames <= (test.targetMs + 5) * 48.0,
                  "extended: corrected playback must drain excess latency before the next interruption");
    }
    // With one extra 10 ms packet of reserve, <=8 ms delivery jitter and
    // clock changes inside +/-800 ppm are expected to remain lossless.
    if (!legacyReference && test.targetMs >= 25 && !test.inputPauseMs && !test.outputPauseMs)
        Check(renderedMissing == 0, "extended: 25/30ms reserve reference must remain lossless");
    if (!legacyReference && test.targetMs == 20 && !test.inputPauseMs && !test.outputPauseMs) {
        if (test.clock == ClockProfile::Reverse800)
            Check(maxBurstMissing <= 48 && g_sharedRebufferSilenceFrames.load() == 0,
                  "extended: extreme clock reversal may not amplify a sub-ms loss into reprime silence");
        else Check(renderedMissing == 0,
                  "extended: moderate clock changes and 8ms jitter must remain lossless at 20ms");
    }
    std::printf("RESULT %s target=%d period=%.3f seed=%u: events=%llu source-missing=%.3fms reprime=%llu intentional=%.3fms total=%.3fms outside-fault=%.3fms max-short-write-burst=%.3fms first/last=%.3f/%.3fs max-refill-wait=%.3fms queue-max/settled=%.3f/%.3fms final=%.3fms correction=%+.1fppm transitions=%u endpoint-gap=%.3fms deadlines=%u failures=%u\n",
        test.name, test.targetMs, periodMs, test.seed, g_underruns.load(),
        g_audioUnderrunFrames.load()/48.0, g_sharedRebuffers.load(),
        g_sharedRebufferSilenceFrames.load()/48.0, renderedMissing/48.0,
        outsideFaultMissing/48.0, maxBurstMissing/48.0, firstLoss/1000, lastLoss/1000,
        maxRebufferWait, maxQueue/48.0, maxSettledQueue/48.0,
        state.filteredQueuedFrames/48.0, state.correctionPpm, autoTransitions,
        endpointMissing/48.0, deadlines, failures - failuresBefore);
    std::fflush(stdout);
}

static void RunRecoveryBoundaries(int target = 20) {
    for (int period : {480, 128})
        for (int pause : {5, 15, 120}) {
            ExtendedCase test{"recovery-boundary"};
            test.targetMs = target;
            test.clock = ClockProfile::FixedZero;
            test.periodFrames = period; test.inputPauseMs = pause; test.discard = true;
            RunExtended(test, 180);
        }
}

static void RunExtendedMatrix(bool longRun, int targetOverride = 0) {
    const int seconds = longRun ? 600 : 240;
    const std::vector<int> targets = targetOverride ? std::vector<int>{targetOverride} :
        std::vector<int>{20, 30};
    for (uint32_t seed : {1u, 0x12345678u, 0xdeadbeefu})
        for (int target : targets)
            for (int period : {480, 128}) {
                ExtendedCase test{"random-arrival-8ms"};
                test.targetMs = target; test.periodFrames = period;
                test.seed = seed; test.jitterMs = 8;
                RunExtended(test, seconds);
            }
    for (auto profile : {ClockProfile::Reverse300, ClockProfile::Reverse800, ClockProfile::Ramp300})
        for (int target : targets) {
            ExtendedCase test{profile == ClockProfile::Reverse300 ? "reverse-300" :
                profile == ClockProfile::Reverse800 ? "reverse-800" : "ramp-300"};
            test.clock = profile; test.targetMs = target;
            RunExtended(test, seconds);
        }
    for (auto mode : {DriftCorrectionMode::Auto, DriftCorrectionMode::Off, DriftCorrectionMode::Resample})
        for (bool discard : {false, true}) {
            ExtendedCase test{mode == DriftCorrectionMode::Auto ? "repeat-input-auto" :
                mode == DriftCorrectionMode::Off ? "repeat-input-off" : "repeat-input-on"};
            test.mode = mode; test.inputPauseMs = 40; test.discard = discard;
            if (targetOverride) test.targetMs = targetOverride;
            test.clock = ClockProfile::FixedZero;
            std::printf("  input policy: %s\n", discard ? "discard" : "delayed burst");
            RunExtended(test, seconds);
        }
    ExtendedCase output{"repeat-output-stall"};
    if (targetOverride) output.targetMs = targetOverride;
    output.outputPauseMs = 40;
    output.clock = ClockProfile::FixedZero;
    RunExtended(output, seconds);
    ExtendedCase combined{"combined-clock-jitter-loss"};
    combined.targetMs = targetOverride ? targetOverride : 30;
    combined.clock = ClockProfile::Reverse300;
    combined.jitterMs = 8; combined.inputPauseMs = 40; combined.discard = true;
    RunExtended(combined, seconds);
    RunRecoveryBoundaries(targetOverride ? targetOverride : 20);
}
