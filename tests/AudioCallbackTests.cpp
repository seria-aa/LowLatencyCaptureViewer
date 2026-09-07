// Hardware-free integration regressions: exercise the actual application
// callbacks, not copies of their startup/minimum-buffer logic.
#include "../src/main.cpp"
#undef fwprintf
#include "../src/audio/AsioOutput.cpp"

#include <cstdlib>
#include "audio/SharedDeadlineMonitor.h"

namespace {
void Require(bool value, const char* message) {
    if (!value) {
        std::fprintf(stderr, "FAILED: %s\n", message);
        std::abort();
    }
}
std::wstring savedMessage;
void SaveMessage(const wchar_t* message) { savedMessage = message; }

void TestExclusiveScanResultLifetime() {
    SettingsDialogState state;
    state.audioEndpoints.resize(2);
    state.audioEndpoints[0].name = L"Mailbox regression endpoint";
    state.exclusiveEndpointResults.resize(2);
    state.exclusiveScanRunning.store(true);
    state.exclusiveProbeThread = std::thread([&state]() {
        ExclusiveEndpointProbeResult result;
        result.endpointIndex = 0;
        result.probe.compatible = true;
        result.probe.requestedFrames = 480;
        result.probe.summary = L"Owned result survives a discarded notification";
        QueueExclusiveEndpointProbeResult(&state, std::move(result));
        // Deliberately omit the UI wake-up, as when its HWND is destroyed.
    });
    state.exclusiveProbeThread.join();
    Require(state.exclusiveScanRunning.load() &&
                state.pendingExclusiveProbeResults.size() == 1 &&
                state.exclusiveScanCompleted == 0,
            "finished scan must retain unread results and stay busy until UI completion");
    StartExclusiveEndpointScan(&state, nullptr, true);
    Require(!state.exclusiveProbeThread.joinable() &&
                state.pendingExclusiveProbeResults.size() == 1 &&
                state.exclusiveScanCompleted == 0,
            "queued old scan completion must block restart without opening any endpoint");
    CompleteExclusiveEndpointScan(&state);
    Require(!state.exclusiveScanRunning.load() &&
                state.pendingExclusiveProbeResults.empty() &&
                state.exclusiveScanCompleted == 1 &&
                state.exclusiveEndpointResults[0].state ==
                    ExclusiveEndpointState::Supported &&
                state.exclusiveEndpointResults[0].recommendedBufferMs == 10 &&
                state.exclusiveEndpointResults[0].summary ==
                    L"Owned result survives a discarded notification",
            "completion must drain state-owned results even when an endpoint wake-up was lost");
    ConsumeExclusiveEndpointProbeResults(&state);
    Require(state.exclusiveScanCompleted == 1,
            "duplicate payload-free notifications must not count results twice");

    // Run the real scan worker with no devices. Completion must remain pending
    // after the worker exits, even if posting to its absent HWND fails.
    SettingsDialogState empty;
    StartExclusiveEndpointScan(&empty, nullptr, true);
    empty.exclusiveProbeThread.join();
    Require(empty.exclusiveScanRunning.load(),
            "real scan worker must not clear running before UI consumes completion");
    StartExclusiveEndpointScan(&empty, nullptr, true);
    Require(!empty.exclusiveProbeThread.joinable(),
            "a completed worker with pending notification must not be replaced");
    CompleteExclusiveEndpointScan(&empty);
    StartExclusiveEndpointScan(&empty, nullptr, true);
    Require(empty.exclusiveProbeThread.joinable(),
            "a fresh scan must be allowed after the previous UI completion");
    CompleteExclusiveEndpointScan(&empty);
    Require(!empty.exclusiveScanRunning.load() &&
                !empty.exclusiveProbeThread.joinable(),
            "UI completion must join the scan worker before publishing idle");

    // An unconsumed result is released with the state, not leaked through a
    // discarded heap pointer in a window-message payload.
    SettingsDialogState discarded;
    ExclusiveEndpointProbeResult unread;
    unread.probe.summary.assign(4096, L'x');
    QueueExclusiveEndpointProbeResult(&discarded, std::move(unread));
    Require(discarded.pendingExclusiveProbeResults.size() == 1,
            "dialog state must own results that will never receive a UI notification");
}

void TestOutputTransitions() {
    const auto savedGeneration = g_outputConfigurationGeneration.load();
    const auto savedDepth = g_outputTransitionDepth;
    const bool savedPending = g_outputResizePending;
    const bool savedManualResize = g_manualResizeInProgress;
    const bool savedAudioOnly = g_settings.audioOnly;
    const HWND savedVideoHost = g_videoHost;
    const HMONITOR savedRelativeMonitor = g_relativeMoveMonitor;
    const auto savedSnapState = g_windowSnapState;
    g_outputConfigurationGeneration.store(100);
    g_outputTransitionDepth = 0;
    g_outputResizePending = false;
    g_manualResizeInProgress = false;
    g_settings.audioOnly = false;
    g_videoHost = nullptr;

    // A null host exercises the real message handlers without creating any
    // window, capture graph, output device, or persistent settings file.
    const auto resize = []() {
        WndProc(nullptr, WM_SIZE, SIZE_RESTORED, MAKELPARAM(1280, 720));
    };
    BeginOutputTransition();
    EndOutputTransition(false);
    Require(g_outputConfigurationGeneration.load() == 100 &&
                g_outputTransitionDepth == 0 && !g_outputResizePending,
            "an empty output transition must not rebuild the renderer");

    BeginOutputTransition();
    resize();
    resize();
    Require(g_outputConfigurationGeneration.load() == 100 &&
                g_outputResizePending,
            "synchronous style-change sizes must wait for transition end");
    EndOutputTransition(true);
    Require(g_outputConfigurationGeneration.load() == 101 &&
                !g_outputResizePending && g_outputTransitionDepth == 0,
            "one fullscreen transition must publish only one output rebuild");

    BeginOutputTransition();
    BeginOutputTransition();
    resize();
    EndOutputTransition(false);
    resize();
    Require(g_outputConfigurationGeneration.load() == 101 &&
                g_outputTransitionDepth == 1 && g_outputResizePending,
            "nested fullscreen exit must not publish halfway through F5");
    EndOutputTransition(true);
    Require(g_outputConfigurationGeneration.load() == 102 &&
                g_outputTransitionDepth == 0 && !g_outputResizePending,
            "nested F5 and fullscreen transitions must coalesce all sizes");

    BeginOutputTransition();
    BeginOutputTransition();
    EndOutputTransition(true);
    Require(g_outputConfigurationGeneration.load() == 102 &&
                g_outputResizePending,
            "an inner explicit rebuild request must remain deferred");
    EndOutputTransition(false);
    Require(g_outputConfigurationGeneration.load() == 103 &&
                !g_outputResizePending,
            "outer completion must preserve an inner explicit rebuild request");

    g_manualResizeInProgress = true;
    BeginOutputTransition();
    resize();
    EndOutputTransition(true);
    resize();
    Require(g_outputConfigurationGeneration.load() == 103 &&
                g_outputTransitionDepth == 0 && g_outputResizePending,
            "completed transitions during a manual drag must remain deferred");
    WndProc(nullptr, WM_EXITSIZEMOVE, 0, 0);
    Require(g_outputConfigurationGeneration.load() == 104 &&
                !g_manualResizeInProgress && !g_outputResizePending,
            "ending a manual drag must publish exactly one pending rebuild");
    WndProc(nullptr, WM_EXITSIZEMOVE, 0, 0);
    Require(g_outputConfigurationGeneration.load() == 104,
            "an idle move completion must not repeat the preceding rebuild");

    WndProc(nullptr, WM_SIZE, SIZE_MINIMIZED, 0);
    Require(g_outputConfigurationGeneration.load() == 104 &&
                !g_outputResizePending,
            "minimization must not rebuild a zero-sized video output");
    resize();
    Require(g_outputConfigurationGeneration.load() == 105,
            "a standalone completed resize must still rebuild the output");
    BeginOutputTransition();
    EndOutputTransition(true);
    Require(g_outputConfigurationGeneration.load() == 106,
            "a later fullscreen transition must not inherit a stale deferral");

    const bool wasFullscreen = g_fullscreen.load();
    for (int repeat = 0; repeat < 3; ++repeat) {
        WndProc(nullptr, WM_KEYDOWN, VK_F11, (LPARAM{1} << 30) | 1);
        Require(g_fullscreen.load() == wasFullscreen &&
                    g_outputConfigurationGeneration.load() == 106 &&
                    g_outputTransitionDepth == 0 && !g_outputResizePending,
                "holding F11 must not toggle fullscreen or rebuild output again");
    }

    g_outputConfigurationGeneration.store(savedGeneration);
    g_outputTransitionDepth = savedDepth;
    g_outputResizePending = savedPending;
    g_manualResizeInProgress = savedManualResize;
    g_settings.audioOnly = savedAudioOnly;
    g_videoHost = savedVideoHost;
    g_relativeMoveMonitor = savedRelativeMonitor;
    g_windowSnapState = savedSnapState;
}
}

int main() {
    TestExclusiveScanResultLifetime();
    TestOutputTransitions();
    g_settings.driftCorrection = DriftCorrectionMode::Off;
    g_audioQueueTargetFrames.store(960);
    g_audioTrackingStartMs.store(UINT64_MAX);
    g_audioMinimumPreRenderFrames.store(UINT32_MAX);
    int16_t output[960]{};
    AsioRenderState asio;
    FillAsioPcm(&asio, output, 480);
    Require(g_audioMinimumPreRenderFrames.load() == UINT32_MAX,
            "ASIO startup and warmup must not publish a minimum");

    const std::vector<int16_t> input(960 * 2, 100);
    g_ring.Push(input.data(), 960);
    asio.audioStarted = true;
    FillAsioPcm(&asio, output, 480);
    Require(g_audioMinimumPreRenderFrames.load() == UINT32_MAX,
            "ASIO running during warmup must not publish a minimum");
    g_ring.Clear();
    g_audioTrackingStartMs.store(0);
    g_ring.Push(input.data(), 960);
    FillAsioPcm(&asio, output, 480);
    Require(g_audioMinimumPreRenderFrames.load() == 960,
            "ASIO minimum must be pre-render, not the 480-frame remainder");
    g_ring.Clear();
    g_audioMinimumPreRenderFrames.store(UINT32_MAX);
    g_ring.Push(input.data(), 960);
    WasapiRenderState wasapi;
    wasapi.queueTargetFrames = 960;
    wasapi.audioStarted = true;
    FillWasapiPcm(&wasapi, output, 480);
    Require(g_audioMinimumPreRenderFrames.load() == 960,
            "WASAPI and ASIO must use the same minimum measurement");
    g_ring.Clear();
    FillWasapiPcm(&wasapi, output, 480);
    Require(g_audioMinimumPreRenderFrames.load() == 0,
            "real post-warmup starvation must still be recorded");
    Require(wasapi.rebuffering, "substantial Shared starvation must re-prime");
    const auto missingBefore = g_audioUnderrunFrames.load();
    const auto silenceBefore = g_sharedRebufferSilenceFrames.load();
    g_ring.Push(input.data(), 480);
    Require(FillWasapiPcm(&wasapi, output, 480).writtenFrames == 0 &&
            wasapi.rebuffering && g_audioUnderrunFrames.load() == missingBefore &&
            g_sharedRebufferSilenceFrames.load() == silenceBefore + 480,
            "re-prime silence must be separate from source underruns");
    g_ring.Push(input.data(), 480);
    Require(FillWasapiPcm(&wasapi, output, 480).writtenFrames == 480 &&
            !wasapi.rebuffering, "re-prime must resume at user reserve without growth");
    g_ring.Clear();

    llcv::audio::SharedDeadlineMonitor deadline;
    Require(!deadline.Observe(0, 576), "first padding sample must not flag startup");
    Require(!deadline.Submitted(0.001, 480), "short fill must meet deadline");
    Require(!deadline.Observe(0.010, 576), "normal engine event must not flag");
    Require(!deadline.Submitted(0.011, 480), "normal release must not flag");
    Require(deadline.Observe(0.050, 0), "late wake with exhausted padding must flag");
    Require(!deadline.Submitted(0.051, 1056), "one wake must not double-count");
    Require(!deadline.Observe(0.060, 576), "normal wake after stall must recover");
    Require(deadline.Submitted(0.080, 480), "slow fill must also flag");

    llcv::audio::SharedDeadlineMonitor emptyOnTime;
    emptyOnTime.Observe(1.0, 0);
    emptyOnTime.Submitted(1.0, 480);
    Require(!emptyOnTime.Observe(1.010, 0),
            "padding zero alone does not prove an output dropout");
    g_settings.audioMode = AudioMode::WasapiShared;
    g_settings.uiLanguage = UiLanguage::English;
    g_sharedRebuffering.store(true);
    Require(BuildRuntimeOsdText(1920,1080).find(L"Refilling PCM reserve") != std::wstring::npos,
            "OSD must distinguish intentional re-prime from normal buffer state");
    g_sharedRebuffering.store(false);
    OnSharedDeadlineSuspected(&wasapi, 0.028, false);
    Require(g_sharedLastOverdueUs.load() == 28000 &&
            BuildRuntimeOsdText(1920,1080).find(L"Output delay suspected") != std::wstring::npos,
            "Shared late-output evidence must reach OSD without requiring PCM underrun");
    g_settings.uiLanguage = UiLanguage::Korean;
    Require(BuildRuntimeOsdText(1920,1080).find(L"출력 지연 의심") != std::wstring::npos,
            "Shared OSD diagnosis must support Korean");

    // Isolated tiny shortages must not repeatedly insert whole silent blocks.
    WasapiRenderState tiny;
    tiny.audioStarted = true;
    tiny.queueTargetFrames = 960;
    for (int attempt = 0; attempt < 3; ++attempt) {
        g_ring.Push(input.data(), 479);
        FillWasapiPcm(&tiny, output, 480);
        Require(!tiny.rebuffering,
                "tiny shortages must not become extra silent output blocks");
    }
    g_ring.Push(input.data(), 240);
    FillWasapiPcm(&tiny, output, 480);
    Require(!tiny.rebuffering, "half a missing block does not justify re-prime");
    g_ring.Push(input.data(), 240);
    FillWasapiPcm(&tiny, output, 480);
    Require(tiny.rebuffering, "one accumulated missing block must re-prime");
    g_ring.Clear();
    WasapiRenderState exclusive;
    exclusive.shared = false;
    exclusive.audioStarted = true;
    FillWasapiPcm(&exclusive, output, 480);
    Require(!exclusive.rebuffering, "Shared reprime must not alter Exclusive policy");

    // The learned clock term must retain reserve rather than a permanent
    // proportional queue error. Ten virtual hours, no hardware or DSP load.
    for (double inputPpm : {-800.0, -300.0, -100.0, 0.0, 100.0, 300.0, 800.0}) {
        llcv::audio::QueueDriftController controller;
        double queued = 976.0;
        double correction = 0.0;
        for (int tick = 0; tick < 3600000; ++tick) {
            correction = controller.Update(queued, 960, 480);
            queued += 480 * (inputPpm - correction) / 1e6;
            Require(std::isfinite(queued) && std::abs(correction) <= 1000.01,
                    "long controller run must remain finite and bounded");
        }
        const double expected = 976 + (std::max)(inputPpm, 0.0) / 2.0;
        Require(std::abs(queued - expected) < 0.1 && std::abs(correction - inputPpm) < 0.1,
                "retain positive-clock P headroom and prevent negative-clock reserve loss");
    }

    llcv::audio::RecoveryPolicy recovery;
    Require(recovery.NextDelay() == 250 &&
            recovery.NextDelay() == 500 &&
            recovery.NextDelay() == 1000 &&
            recovery.NextDelay() == 0,
            "repeated failures must stop after three delayed retries");
    recovery.ObserveSuccessfulRuntime(0);
    Require(recovery.NextDelay() == 0,
            "slow failed setup and backoff must not restore recovery budget");
    recovery.ObserveSuccessfulRuntime(15000);
    recovery.ObserveSuccessfulRuntime(15000);
    Require(recovery.NextDelay() == 0,
            "separate short sessions must not add up to a stable session");
    recovery.ObserveSuccessfulRuntime(29999);
    Require(recovery.NextDelay() == 0,
            "a session shorter than thirty seconds must not restore the budget");
    recovery.ObserveSuccessfulRuntime(30000);
    Require(recovery.NextDelay() == 250,
            "thirty seconds of successful runtime must restore recovery budget");

    using namespace llcv::asio;
    g_acceptRequests.store(false);
    g_restartRequested.store(false);
    Require(AsioMessage(kAsioResetRequest, 0, nullptr, nullptr) == 0 &&
            !g_restartRequested.load(), "inactive ASIO must not accept resets");
    g_acceptRequests.store(true);
    for (long request : {kAsioResetRequest, kAsioResyncRequest, kAsioLatenciesChanged}) {
        g_restartRequested.store(false);
        Require(AsioMessage(request, 0, nullptr, nullptr) == 1 &&
                g_restartRequested.load(),
                "accepted driver notifications must reach the owner");
    }
    g_restartRequested.store(false);
    AsioSampleRateChanged(48000);
    Require(!g_restartRequested.load(), "48 kHz confirmation must not restart ASIO");
    AsioSampleRateChanged(44100);
    Require(g_restartRequested.load(), "rate changes must not silently change playback speed");
    Require(AsioMessage(kAsioSelectorSupported, kAsioSupportsTimeInfo, nullptr, nullptr) == 0,
            "ASIO must not advertise an unimplemented time-info callback");
    g_acceptRequests.store(false);

    llcv::diagnostics::LogMessage(SaveMessage, L"buffer %u frames", 960u);
    Require(savedMessage == L"buffer 960 frames",
            "formatted module diagnostics must reach the supplied log sink");
    return 0;
}
