// Shared regression: real PCM callback / DSP, virtual clock.
// --long runs the soak matrix; default limits each case to 240 seconds.
// Only wall time is replaced; no device is enumerated, opened, or changed.
#include <windows.h>
#include <cstdint>
static uint64_t replayMilliseconds = 1000;
static ULONGLONG WINAPI ReplayClock() { return replayMilliseconds; }
#define GetTickCount64 ReplayClock
#include "../src/main.cpp"
#undef GetTickCount64
#undef fwprintf
#include <cstdio>
#include "audio/SharedDeadlineMonitor.h"
static unsigned failures = 0;
static void Check(bool ok, const char* what) {
    if (!ok) { std::fprintf(stderr, "FAILED: %s\n", what); ++failures; }
}

struct Scenario {
    const char* name;
    int seconds, targetMs, periodFrames;
    double inputPpm;
    DriftCorrectionMode mode;
    int inputPauseMs = 0;
    int outputPauseMs = 0;
    bool discardPaused = false;
    int packetFrames = 480;
    bool jitter = false;
};

void Run(const Scenario& test) {
    g_running.store(true);
    g_settings.driftCorrection = test.mode;
    g_audioTrackingStartMs.store(6000);
    g_audioCapturePacketFrames.store(test.packetFrames);
    g_audioMinimumPreRenderFrames.store(UINT32_MAX);
    g_underruns.store(0);
    g_sharedRebuffers.store(0);
    g_sharedRebufferSilenceFrames.store(0);
    g_sharedRebuffering.store(false);
    llcv::audio::SharedDeadlineMonitor monitor;
    unsigned deadlineSuspicions = 0;
    g_audioUnderrunFrames.store(0);
    g_audioResamplerUnderruns.store(0);
    g_audioLatePacketUnderruns.store(0);
    g_ring.Clear();
    g_audioResamplerFrames.store(0);
    WasapiRenderState state;
    state.queueTargetFrames = test.targetMs * 48;
    state.autoCorrectionActive = test.mode == DriftCorrectionMode::Resample;
    const size_t capacity = test.periodFrames * 2 + 96;
    state.driftResampler.Prepare(capacity);
    const uint64_t oldOverruns = g_ring.Overruns();
    std::vector<int16_t> packet(test.packetFrames*2,1000), output(capacity*2);
    const double periodMs = test.periodFrames / 48.0;
    const double capturePeriod = test.packetFrames / 48.0 / (1.0 + test.inputPpm / 1e6);
    double nextCaptureMs = 2;
    size_t padding = capacity;
    uint64_t endpointMissing = 0, appMissingAfterPause = 0;
    double firstUnderrun = -1, lastUnderrun = -1, engagedAt = -1;
    double missingFirst = -1, missingLast = -1;
    uint32_t minPre = UINT32_MAX, maxPre = 0;
    size_t minRecoveryPre = SIZE_MAX;
    for (uint64_t step = 1; step * periodMs <= test.seconds * 1000.0; ++step) {
        const double timeMs = step * periodMs;
        replayMilliseconds = 1000 + static_cast<uint64_t>(timeMs);
        if (test.periodFrames > padding && timeMs > 5000) {
            endpointMissing += test.periodFrames - padding;
            if (missingFirst < 0) missingFirst = timeMs;
            missingLast = timeMs;
        }
        padding = padding > static_cast<size_t>(test.periodFrames)
            ? padding - test.periodFrames : 0;
        const bool inputPaused = (test.inputPauseMs && timeMs >= 60000 &&
            timeMs < 60000 + test.inputPauseMs) ||
            (test.jitter && step % 97 == 0);
        if (!inputPaused) {
            while (nextCaptureMs <= timeMs) {
                g_ring.Push(packet.data(),test.packetFrames);
                g_audioLastCaptureCallbackMs.store(replayMilliseconds);
                nextCaptureMs += capturePeriod;
            }
        } else if (test.discardPaused) {
            while (nextCaptureMs <= timeMs) nextCaptureMs += capturePeriod;
        }
        if (test.outputPauseMs && timeMs >= 60000 &&
            timeMs < 60000 + test.outputPauseMs) continue;
        const size_t writable = capacity - padding;
        if (monitor.Observe(timeMs / 1000.0, static_cast<uint32_t>(padding)) && timeMs > 5000)
            ++deadlineSuspicions;
        const auto result = FillWasapiPcm(&state, output.data(), writable);
        monitor.Submitted(timeMs / 1000.0, static_cast<uint32_t>(writable));
        padding += writable; // Production writes PCM + zero-fill for shortages.
        if (state.autoCorrectionActive && engagedAt < 0) engagedAt = timeMs;
        Check(result.writtenFrames <= writable, "write must fit requested buffer");
        Check(std::abs(state.correctionPpm) <= 1000.01, "correction must remain bounded");
        if (timeMs > 5000 && state.audioStarted) {
            minPre = (std::min)(minPre, static_cast<uint32_t>(result.availableBeforeRender));
            maxPre = (std::max)(maxPre, static_cast<uint32_t>(result.availableBeforeRender));
            if (result.writtenFrames < writable) {
                if (firstUnderrun < 0) firstUnderrun = timeMs;
                lastUnderrun = timeMs;
                if (timeMs > 60000 + test.inputPauseMs)
                    appMissingAfterPause += writable - result.writtenFrames;
            }
            if (timeMs >= 65000 && timeMs < 66000)
                minRecoveryPre = (std::min)(minRecoveryPre,result.availableBeforeRender);
        }
    }
    std::printf("%s: duration=%ds target=%dms period=%.3fms ppm=%+.0f; app-events=%llu missing=%.3fms first/last=%.3f/%.3fs; endpoint-gap=%.3fms first/last=%.3f/%.3fs; overrun=%llu; pre-min/max=%.3f/%.3fms final-pre-filter=%.3fms correction=%+.1fppm auto-at=%.3fs; after-pause-missing=%.3fms recovery-min-at65s=%.3fms\n",
        test.name,test.seconds,test.targetMs,periodMs,test.inputPpm,
        static_cast<unsigned long long>(g_underruns.load()),
        g_audioUnderrunFrames.load()/48.0,firstUnderrun/1000,lastUnderrun/1000,
        endpointMissing/48.0,missingFirst/1000,missingLast/1000,
        static_cast<unsigned long long>(g_ring.Overruns()-oldOverruns),
        minPre/48.0,maxPre/48.0,state.filteredQueuedFrames/48.0,
        state.correctionPpm,engagedAt/1000,appMissingAfterPause/48.0,
        minRecoveryPre==SIZE_MAX?-1:minRecoveryPre/48.0);
    std::printf("  reprime=%llu, intentional-silence=%.3fms, deadline-suspicions=%u\n",
        g_sharedRebuffers.load(),g_sharedRebufferSilenceFrames.load()/48.0,deadlineSuspicions);
    Check(g_ring.Overruns() == oldOverruns, "no queue overflow in test envelope");
    if (!test.inputPauseMs && !test.outputPauseMs && !test.jitter && test.targetMs >= 20 &&
        test.packetFrames == 480 && std::abs(test.inputPpm) <= 300)
        Check(g_underruns.load() == 0, "steady 20ms/10ms-packet playback must not underrun");
    // At -800 ppm Auto's cold-start observation still allows one sub-ms
    // shortfall. Preserve this known limit explicitly; no recurring losses
    // or additional reprime silence are acceptable after the first 30 s.
    if (!test.jitter && test.inputPpm < -300) {
        Check(g_underruns.load() <= 1 && g_audioUnderrunFrames.load() <= 48 &&
              lastUnderrun <= 30000 && g_sharedRebufferSilenceFrames.load() == 0,
              "large negative drift may only have a bounded cold-start shortfall");
    }
    if (test.outputPauseMs) {
        Check(endpointMissing > 0 && deadlineSuspicions > 0,
              "output stall must be diagnosed independently of PCM starvation");
    } else Check(deadlineSuspicions == 0, "normal output pacing must not be flagged");
    if (test.discardPaused) {
        Check(g_sharedRebuffers.load() > 0, "lost packets must trigger bounded reprime");
        Check(minRecoveryPre >= static_cast<size_t>(test.targetMs * 48),
              "lost-packet recovery must restore the requested reserve");
    }
    if (test.targetMs == 15) {
        Check(g_sharedRebufferSilenceFrames.load() == 0,
              "isolated shortfalls at a too-small target must not add reprime silence");
    }
    if (test.jitter && test.inputPpm > 0) {
        Check(g_underruns.load() == 0,
              "do not regress legacy positive-clock jitter headroom");
    }
    std::fflush(stdout);
}
#include "SharedReplayExtended.inl"

int main(int argc,char** argv) {
    if (argc > 1 && std::strcmp(argv[1], "--target25") == 0) {
        RunExtendedMatrix(argc > 2 && std::strcmp(argv[2], "--long") == 0, 25);
        return failures ? 1 : 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "--boundaries") == 0) {
        RunRecoveryBoundaries();
        return failures ? 1 : 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "--extended") == 0) {
        RunExtendedMatrix(argc > 2 && std::strcmp(argv[2], "--long") == 0);
        return failures ? 1 : 0;
    }
    const Scenario cases[] = {
        {"steady-plus100",3600,20,480,100,DriftCorrectionMode::Auto},
        {"steady-minus100",3600,20,480,-100,DriftCorrectionMode::Auto},
        {"steady-minus300",1800,20,480,-300,DriftCorrectionMode::Auto},
        {"low-period",1800,20,128,-100,DriftCorrectionMode::Auto},
        {"input-stall-auto",120,20,480,0,DriftCorrectionMode::Auto,40},
        {"input-stall-off",120,20,480,0,DriftCorrectionMode::Off,40},
        {"output-stall",120,20,480,0,DriftCorrectionMode::Auto,0,40},
        {"lower-target",1800,15,480,-100,DriftCorrectionMode::Auto},
        {"input-stall-resample",120,20,480,0,DriftCorrectionMode::Resample,40},
        {"more-margin",1800,25,480,-300,DriftCorrectionMode::Auto},
        {"forced-minus300",600,20,480,-300,DriftCorrectionMode::Resample},
        {"lost-packets-auto",120,20,480,0,DriftCorrectionMode::Auto,40,0,true},
        {"lost-packets-off",120,20,480,0,DriftCorrectionMode::Off,40,0,true},
        {"larger-input-packets",600,20,480,-100,DriftCorrectionMode::Auto,0,0,false,960},
        {"fast-clock",600,20,480,800,DriftCorrectionMode::Auto},
        {"slow-clock",600,20,480,-800,DriftCorrectionMode::Auto},
        {"input-jitter",600,20,480,-100,DriftCorrectionMode::Auto,0,0,false,480,true},
        {"forced-jitter",600,20,480,100,DriftCorrectionMode::Resample,0,0,false,480,true},
    };
    const bool longRun = argc > 1 && std::strcmp(argv[1], "--long") == 0;
    for (auto test : cases) {
        if (!longRun) test.seconds = (std::min)(test.seconds, 240);
        Run(test);
    }
    return failures ? 1 : 0;
}
