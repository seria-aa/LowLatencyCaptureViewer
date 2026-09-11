// Hardware-free integration regressions: exercise the actual application
// callbacks, not copies of their startup/minimum-buffer logic.
#define LLCV_GPU_DIAGNOSTICS
#include "../src/main.cpp"
#undef fwprintf
#include "../src/audio/AsioOutput.cpp"

#include <cstdlib>
#include <d3d11sdklayers.h>
#include <set>
#include "audio/SharedDeadlineMonitor.h"
#include "FakeVideoPin.h"

void TestStartupWaitBoundary() {
    using Clock = std::chrono::steady_clock;
    const auto start = Clock::time_point{};
    const auto deadline = start + std::chrono::seconds(10);
    for (int ms = 0; ms <= 20000; ++ms) {
        const auto now = start + std::chrono::milliseconds(ms);
        // The production loop calls this predicate only following a non-event
        // wait. This is a boundary test, not a full capture-graph simulation.
        if (StartupInputWaitExpired(false, now, deadline) != (ms >= 10000) ||
            StartupInputWaitExpired(true, now, deadline)) std::abort();
    }
    // Valid input with no successful presentation (OCCLUDED) must not time out.
    if (StartupInputWaitExpired(true, deadline, deadline)) std::abort();
    std::puts("Startup boundary: 20001 timestamps x pre/post input states passed.");
}

namespace {
void Require(bool value, const char* message) {
    if (!value) {
        std::fprintf(stderr, "FAILED: %s\n", message);
        std::abort();
    }
}
std::wstring savedMessage;
void SaveMessage(const wchar_t* message) { savedMessage = message; }

DWORD settingsProbeThread = 0;
int settingsProbeCalls = 0;
bool settingsProbeFails = false;
std::vector<PixelFormatSupport> SimulatedSettingsProbe(
    const std::wstring& id, int width, int height, HRESULT* status) {
    Require(GetCurrentThreadId() == settingsProbeThread,
            "video capability probe stays synchronous on settings caller thread");
    ++settingsProbeCalls;
    FakeVideoPin pin;
    pin.countFails = settingsProbeFails;
    pin.modes = {{id == L"device-A" ? 120 : 60, width, height}};
    return llcv::video::ProbePixelFormats(&pin, width, height, status);
}

void TestSettingsCapabilityRefresh() {
    const auto saved = g_settings;
    g_settings.videoFrameRate = 0;
    g_settings.pixelFormat = VideoPixelFormat::Auto;
    HWND parent = CreateWindowExW(0, L"STATIC", L"Hidden capability replay", 0,
        0, 0, 320, 240, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    Require(parent != nullptr, "create hidden settings replay parent");
    {
        SettingsDialogState state;
        auto control = [&](const wchar_t* type) {
            HWND child = CreateWindowExW(0, type, L"", WS_CHILD |
                (std::wcscmp(type, L"COMBOBOX") == 0 ? CBS_DROPDOWNLIST : 0),
                0, 0, 200, 100, parent, nullptr, GetModuleHandleW(nullptr), nullptr);
            Require(child != nullptr, "create settings replay control");
            return child;
        };
        state.captureDeviceCombo = control(L"COMBOBOX");
        state.videoCombo = control(L"COMBOBOX");
        state.pixelFormatCombo = control(L"COMBOBOX");
        state.frameRateCombo = control(L"COMBOBOX");
        state.videoCapabilityStatus = control(L"STATIC");
        state.startButton = control(L"BUTTON");
        state.captureDevices.resize(2);
        state.captureDevices[0].id = L"device-A";
        state.captureDevices[1].id = L"device-B";
        for (const auto* label : {L"Auto", L"A", L"B"})
            SendMessageW(state.captureDeviceCombo, CB_ADDSTRING, 0,
                         reinterpret_cast<LPARAM>(label));
        for (size_t i = 0; i < ARRAYSIZE(kVideoPresets); ++i)
            SendMessageW(state.videoCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"size"));
        g_testVideoCapabilityProbe = SimulatedSettingsProbe;
        settingsProbeThread = GetCurrentThreadId();
        settingsProbeCalls = 0;
        for (int iteration = 0; iteration < 1000; ++iteration) {
            SendMessageW(state.captureDeviceCombo, CB_SETCURSEL, 1 + iteration % 2, 0);
            SendMessageW(state.videoCombo, CB_SETCURSEL, iteration % ARRAYSIZE(kVideoPresets), 0);
            settingsProbeFails = iteration % 5 == 0;
            PopulatePixelFormatCombo(&state);
            Require(settingsProbeCalls == iteration + 1,
                    "each selection applies exactly one fresh synchronous query");
            if (settingsProbeFails) {
                Require(FAILED(state.videoCapabilityQueryStatus) && state.pixelFormats.empty() && !IsWindowEnabled(state.startButton) &&
                        !IsWindowEnabled(state.frameRateCombo),
                        "query failure has explicit error status and disables start");
            } else {
                Require(SUCCEEDED(state.videoCapabilityQueryStatus) && state.pixelFormats.size() == 1 &&
                        state.pixelFormats[0].selectedFps == (iteration % 2 ? 60 : 120) &&
                        IsWindowEnabled(state.startButton) && IsWindowEnabled(state.frameRateCombo),
                        "device/resolution change or recovery replaces stale modes and re-enables UI");
            }
        }
        g_testVideoCapabilityProbe = nullptr;
    }
    DestroyWindow(parent);
    g_settings = saved;
    std::puts("Settings capability refresh: 1000 device/resolution/failure/recovery changes passed.");
}

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
    const auto savedTransition = g_outputTransition;
    const bool savedAudioOnly = g_settings.audioOnly;
    const HWND savedVideoHost = g_videoHost;
    const HMONITOR savedRelativeMonitor = g_relativeMoveMonitor;
    const auto savedSnapState = g_windowSnapState;
    g_outputConfigurationGeneration.store(100);
    g_outputTransition = {};
    g_settings.audioOnly = false;
    g_videoHost = nullptr;

    // A null host exercises the real message handlers without creating any
    // window, capture graph, output device, or persistent settings file.
    int resizeWidth = 1280;
    const auto resize = [&]() {
        WndProc(nullptr, WM_SIZE, SIZE_RESTORED, MAKELPARAM(resizeWidth++, 720));
    };
    BeginOutputTransition();
    EndOutputTransition(false);
    Require(g_outputConfigurationGeneration.load() == 100 &&
                g_outputTransition.Depth() == 0 && !g_outputTransition.Pending(),
            "an empty output transition must not rebuild the renderer");

    BeginOutputTransition();
    resize();
    resize();
    Require(g_outputConfigurationGeneration.load() == 100 &&
                g_outputTransition.Pending(),
            "synchronous style-change sizes must wait for transition end");
    EndOutputTransition(true);
    Require(g_outputConfigurationGeneration.load() == 101 &&
                !g_outputTransition.Pending() && g_outputTransition.Depth() == 0,
            "one fullscreen transition must publish only one output rebuild");

    BeginOutputTransition();
    BeginOutputTransition();
    resize();
    EndOutputTransition(false);
    resize();
    Require(g_outputConfigurationGeneration.load() == 101 &&
                g_outputTransition.Depth() == 1 && g_outputTransition.Pending(),
            "nested fullscreen exit must not publish halfway through F5");
    EndOutputTransition(true);
    Require(g_outputConfigurationGeneration.load() == 102 &&
                g_outputTransition.Depth() == 0 && !g_outputTransition.Pending(),
            "nested F5 and fullscreen transitions must coalesce all sizes");

    BeginOutputTransition();
    BeginOutputTransition();
    EndOutputTransition(true);
    Require(g_outputConfigurationGeneration.load() == 102 &&
                g_outputTransition.Pending(),
            "an inner explicit rebuild request must remain deferred");
    EndOutputTransition(false);
    Require(g_outputConfigurationGeneration.load() == 103 &&
                !g_outputTransition.Pending(),
            "outer completion must preserve an inner explicit rebuild request");

    g_outputTransition.SetManualResize(true);
    BeginOutputTransition();
    resize();
    EndOutputTransition(true);
    resize();
    Require(g_outputConfigurationGeneration.load() == 103 &&
                g_outputTransition.Depth() == 0 && g_outputTransition.Pending(),
            "completed transitions during a manual drag must remain deferred");
    WndProc(nullptr, WM_EXITSIZEMOVE, 0, 0);
    Require(g_outputConfigurationGeneration.load() == 104 &&
                !g_outputTransition.ManualResize() && !g_outputTransition.Pending(),
            "ending a manual drag must publish exactly one pending rebuild");
    WndProc(nullptr, WM_EXITSIZEMOVE, 0, 0);
    Require(g_outputConfigurationGeneration.load() == 104,
            "an idle move completion must not repeat the preceding rebuild");

    WndProc(nullptr, WM_SIZE, SIZE_MINIMIZED, 0);
    Require(g_outputConfigurationGeneration.load() == 104 &&
                !g_outputTransition.Pending(),
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
                    g_outputTransition.Depth() == 0 && !g_outputTransition.Pending(),
                "holding F11 must not toggle fullscreen or rebuild output again");
    }

    for (int cycle = 0; cycle < 1000; ++cycle) {
        const auto before = g_outputConfigurationGeneration.load();
        BeginOutputTransition();
        for (int event = 0; event < 5; ++event) resize();
        EndOutputTransition(false);
        Require(g_outputConfigurationGeneration.load() == before + 1,
                "repeated transition sizes coalesce into one generation");
        WndProc(nullptr, WM_SIZE, SIZE_RESTORED, MAKELPARAM(resizeWidth - 1, 720));
        WndProc(nullptr, WM_SIZE, SIZE_RESTORED, MAKELPARAM(resizeWidth - 1, 720));
        Require(g_outputConfigurationGeneration.load() == before + 1,
                "identical standalone sizes do not request redundant rebuilds");
        DirectD3D11Renderer observer;
        observer.outputConfigurationGeneration = before;
        Require(observer.outputConfigurationChanged(), "older renderer generation remains invalid");
        observer.outputConfigurationGeneration = before + 1;
        Require(!observer.outputConfigurationChanged(), "current renderer generation is recognized");
    }

    g_outputConfigurationGeneration.store(savedGeneration);
    g_outputTransition = savedTransition;
    g_settings.audioOnly = savedAudioOnly;
    g_videoHost = savedVideoHost;
    g_relativeMoveMonitor = savedRelativeMonitor;
    g_windowSnapState = savedSnapState;
}
}

void TestPresentationPolicy() {
    using namespace llcv::presentation;
    const auto saved = g_settings;
    for (auto mode : {Mode::AllowTearing, Mode::VSync, Mode::Compatibility}) {
        for (bool tearing : {false, true}) {
            const auto desc = Description(mode, 1920, 1080, false, tearing);
            Require(desc.Width == 1920 && desc.Height == 1080 &&
                    desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM &&
                    desc.BufferCount == 2, "output geometry and format unchanged");
            if (IsCompatibility(mode)) {
                Require(desc.SwapEffect == DXGI_SWAP_EFFECT_DISCARD &&
                        desc.Flags == 0 && UsesVSync(mode),
                        "Blt output must not use flip-only flags or immediate presentation");
            } else {
                Require(desc.SwapEffect == DXGI_SWAP_EFFECT_FLIP_DISCARD &&
                        (desc.Flags & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT) &&
                        !!(desc.Flags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) == tearing,
                        "existing flip swapchain policy remains unchanged");
            }
        }
        g_settings.presentationMode = mode;
        for (auto language : {UiLanguage::Korean, UiLanguage::English}) {
            g_settings.uiLanguage = language;
            const auto osd = BuildRuntimeOsdText(1920, 1080);
            Require(osd.find(PathName(mode)) != std::wstring::npos,
                    "diagnostics must identify the selected output path");
        }
    }
    g_settings = saved;
}

// Explicit opt-in GPU smoke test: synthetic pixels only, hidden HWND, no
// capture/audio devices and no reads/writes to the user's settings or logs.
int TestPresentationGpu() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    HWND hwnd = CreateWindowExW(0, L"STATIC", L"Presentation smoke",
        WS_OVERLAPPEDWINDOW, 0, 0, 320, 240, nullptr, nullptr,
        GetModuleHandleW(nullptr), nullptr);
    Require(hwnd != nullptr, "create hidden GPU smoke window");
    g_settings.pixelPerfect = false;
    std::vector<BYTE> pixels(64 * 64 * 3 / 2, 128);
    for (auto mode : {PresentationMode::AllowTearing, PresentationMode::VSync,
                      PresentationMode::Compatibility, PresentationMode::VSync}) {
        g_settings.presentationMode = mode;
        DirectD3D11Renderer renderer;
        for (int cycle = 0; cycle < 2; ++cycle) {
            SetWindowPos(hwnd, nullptr, 0, 0, 320 + cycle * 32, 240 + cycle * 32,
                         SWP_NOZORDER | SWP_NOMOVE | SWP_NOACTIVATE);
            const auto hr = renderer.initialize(hwnd, 64, 64, 60, VideoPixelFormat::Nv12);
            std::printf("GPU init mode=%d cycle=%d HRESULT=0x%08lX\n",
                        static_cast<int>(mode), cycle, static_cast<unsigned long>(hr));
            Require(SUCCEEDED(hr), "initialize actual D3D11 output path");
            DXGI_SWAP_CHAIN_DESC1 desc{};
            Require(SUCCEEDED(renderer.swapChain->GetDesc1(&desc)) &&
                    desc.SwapEffect == (mode == PresentationMode::Compatibility
                        ? DXGI_SWAP_EFFECT_DISCARD : DXGI_SWAP_EFFECT_FLIP_DISCARD),
                    "actual swapchain uses requested path");
            renderer.upload(pixels.data(), 64);
            const auto presented = renderer.presentUploaded();
            Require(SUCCEEDED(presented), "synthetic frame upload and presentation");
        }
    }
    g_settings.presentationMode = PresentationMode::Compatibility;
    DirectD3D11Renderer hdr;
    Require(hdr.initialize(hwnd, 64, 64, 60, VideoPixelFormat::P010, true) ==
            DXGI_ERROR_UNSUPPORTED, "reject HDR10 instead of misinterpreting it as SDR");
    DestroyWindow(hwnd);
    CoUninitialize();
    return 0;
}

int TestPresentationRecreateReplay() {
    Require(SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)), "initialize GPU replay COM");
    HWND hwnd = CreateWindowExW(0, L"STATIC", L"Hidden rebuild replay",
        WS_OVERLAPPEDWINDOW, 0, 0, 320, 240, nullptr, nullptr,
        GetModuleHandleW(nullptr), nullptr);
    Require(hwnd != nullptr, "create hidden recreation replay window");
    IMemAllocator* allocator = nullptr;
    Require(SUCCEEDED(CoCreateInstance(CLSID_MemoryAllocator, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&allocator))), "create in-process DirectShow sample allocator");
    ALLOCATOR_PROPERTIES requested{4, 64 * 64 * 3 / 2, 1, 0}, actual{};
    Require(SUCCEEDED(allocator->SetProperties(&requested, &actual)) &&
        actual.cbBuffer >= requested.cbBuffer && SUCCEEDED(allocator->Commit()), "allocate synthetic NV12 samples");
    HANDLE ready = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    Require(ready != nullptr, "create live-mailbox GPU replay event");
    std::atomic<bool> stop{false};
    std::atomic<unsigned> produced{0};
    unsigned frames = 0, diagnostics = 0, occluded = 0;
    {
        llcv::capture::LatestVideoSample slot(requested.cbBuffer, ready);
        auto* callback = new llcv::capture::VideoSampleGrabberCallback(&slot);
        std::thread producer([&] {
            while (!stop.load()) {
                IMediaSample* sample = nullptr;
                if (FAILED(allocator->GetBuffer(&sample, nullptr, nullptr, 0))) break;
                BYTE* pixels = nullptr;
                if (SUCCEEDED(sample->GetPointer(&pixels))) {
                    std::memset(pixels, 128, requested.cbBuffer);
                    sample->SetActualDataLength(requested.cbBuffer);
                    callback->SampleCB(0, sample);
                    ++produced;
                }
                sample->Release();
                Sleep(1);
            }
        });
        g_settings.pixelPerfect = false;
        DirectD3D11Renderer renderer;
        for (int cycle = 0; cycle < 60; ++cycle) {
            g_settings.presentationMode = cycle % 3 == 0 ? PresentationMode::AllowTearing :
                cycle % 3 == 1 ? PresentationMode::VSync : PresentationMode::Compatibility;
            SetWindowPos(hwnd, nullptr, 0, 0, 320 + (cycle % 4) * 32,
                         240 + (cycle % 4) * 24, SWP_NOZORDER | SWP_NOMOVE | SWP_NOACTIVATE);
            g_outputConfigurationGeneration.fetch_add(1);
            Require(SUCCEEDED(renderer.initialize(hwnd, 64, 64, 60, VideoPixelFormat::Nv12)),
                    "recreate renderer while callbacks continue publishing");
            Require(!renderer.outputConfigurationChanged(), "recreated output acknowledges current generation");
            // Check generation changes that arrive after initialization as well.
            g_outputConfigurationGeneration.fetch_add(1);
            Require(renderer.outputConfigurationChanged(), "later output change remains visible to renderer");
            Require(WaitForSingleObject(ready, 2000) == WAIT_OBJECT_0, "capture mailbox remains live across rebuild");
            int64_t arrival = 0;
            auto* sample = slot.TakeLatest(arrival);
            Require(sample != nullptr, "take valid sample after rebuild");
            BYTE* pixels = nullptr;
            Require(SUCCEEDED(sample->GetPointer(&pixels)), "read synthetic pixels");
            renderer.upload(pixels, 64);
            sample->Release();
            const auto hr = renderer.presentUploaded();
            Require(SUCCEEDED(hr), "present after recreation without failure");
            occluded += hr == DXGI_STATUS_OCCLUDED;
            ++frames;
            ID3D11InfoQueue* queue = nullptr;
            Require(SUCCEEDED(renderer.device->QueryInterface(IID_PPV_ARGS(&queue))), "read GPU debug queue");
            for (UINT64 i = 0; i < queue->GetNumStoredMessagesAllowedByRetrievalFilter(); ++i) {
                SIZE_T length = 0;
                queue->GetMessage(i, nullptr, &length);
                std::vector<BYTE> buffer(length);
                auto* message = reinterpret_cast<D3D11_MESSAGE*>(buffer.data());
                if (SUCCEEDED(queue->GetMessage(i, message, &length)) &&
                    message->Severity <= D3D11_MESSAGE_SEVERITY_WARNING) {
                    ++diagnostics;
                    std::fprintf(stderr, "D3D replay: %s\n", message->pDescription);
                }
            }
            queue->Release();
        }
        stop.store(true);
        producer.join();
        callback->Release();
    }
    CloseHandle(ready);
    allocator->Decommit(); allocator->Release();
    DestroyWindow(hwnd);
    CoUninitialize();
    std::printf("Recreate replay: %u rebuild/present cycles, %u concurrent samples, %u occluded, %u GPU warnings/errors.\n",
                frames, produced.load(), occluded, diagnostics);
    return diagnostics ? 1 : 0;
}

int TestPresentationDebug() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    HWND hwnd = CreateWindowExW(0, L"STATIC", L"GPU debug validation",
        WS_OVERLAPPEDWINDOW, 0, 0, 1280, 720, nullptr, nullptr,
        GetModuleHandleW(nullptr), nullptr);
    Require(hwnd != nullptr, "create hidden diagnostic window");
    g_settings.pixelPerfect = false;
    unsigned warnings = 0, errors = 0;
    std::set<int> printedMessages;
    for (auto mode : {PresentationMode::AllowTearing, PresentationMode::VSync,
                      PresentationMode::Compatibility}) {
        for (auto format : {VideoPixelFormat::Nv12, VideoPixelFormat::Yuy2}) {
            g_settings.presentationMode = mode;
            DirectD3D11Renderer renderer;
            const auto initialized = renderer.initialize(hwnd, 1920, 1080, 60, format);
            if (FAILED(initialized)) {
                std::printf("DEBUG init failed 0x%08lX; debug layer or hardware may be unavailable\n",
                            static_cast<unsigned long>(initialized));
                DestroyWindow(hwnd);
                CoUninitialize();
                return 2;
            }
            ID3D11InfoQueue* queue = nullptr;
            Require(SUCCEEDED(renderer.device->QueryInterface(IID_PPV_ARGS(&queue))),
                    "D3D11 debug info queue must be available");
            const UINT stride = format == VideoPixelFormat::Yuy2 ? 3840 : 1920;
            const size_t bytes = format == VideoPixelFormat::Yuy2
                ? 1920 * 1080 * 2 : 1920 * 1080 * 3 / 2;
            std::vector<BYTE> pixels(bytes, 128);
            double total[4]{}, maximum[4]{};
            unsigned occluded = 0;
            for (unsigned frame = 0; frame < 120; ++frame) {
                g_osdVisible.store((frame / 30) % 2 != 0);
                // A hidden window would otherwise skip all work after the first
                // OCCLUDED result. Force the processing call, NOT visibility,
                // to validate repeated GPU writes and overlay transitions.
                renderer.occluded = false;
                renderer.occlusionLogged = true;
                const auto start = std::chrono::steady_clock::now();
                renderer.upload(pixels.data(), stride);
                const double uploadUs = std::chrono::duration<double, std::micro>(
                    std::chrono::steady_clock::now() - start).count();
                const auto hr = renderer.presentUploaded();
                if (hr == DXGI_STATUS_OCCLUDED) ++occluded;
                Require(SUCCEEDED(hr), "debug frame processing must succeed");
                const double durations[] = {uploadUs, renderer.diagnosticVideoUs,
                    renderer.diagnosticOverlayUs, renderer.diagnosticPresentUs};
                for (int i = 0; i < 4; ++i) {
                    total[i] += durations[i];
                    maximum[i] = (std::max)(maximum[i], durations[i]);
                }
            }
            renderer.context->Flush();
            const UINT64 count = queue->GetNumStoredMessagesAllowedByRetrievalFilter();
            for (UINT64 i = 0; i < count; ++i) {
                SIZE_T bytesNeeded = 0;
                queue->GetMessage(i, nullptr, &bytesNeeded);
                std::vector<BYTE> storage(bytesNeeded);
                auto* message = reinterpret_cast<D3D11_MESSAGE*>(storage.data());
                if (FAILED(queue->GetMessage(i, message, &bytesNeeded))) continue;
                if (message->Severity > D3D11_MESSAGE_SEVERITY_WARNING) continue;
                if (message->Severity == D3D11_MESSAGE_SEVERITY_WARNING) ++warnings;
                else ++errors;
                if (printedMessages.insert(static_cast<int>(message->ID)).second) {
                    std::printf("D3D11 severity=%d id=%d: %s\n",
                        static_cast<int>(message->Severity),
                        static_cast<int>(message->ID), message->pDescription);
                }
            }
            queue->Release();
            std::printf("mode=%d format=%d frames=120 occluded=%u; CPU API duration avg/max us: "
                        "upload %.1f/%.1f video %.1f/%.1f overlay %.1f/%.1f present %.1f/%.1f\n",
                        static_cast<int>(mode), static_cast<int>(format), occluded,
                        total[0]/120, maximum[0], total[1]/120, maximum[1],
                        total[2]/120, maximum[2], total[3]/120, maximum[3]);
        }
    }
    g_osdVisible.store(false);
    DestroyWindow(hwnd);
    CoUninitialize();
    std::printf("D3D11 totals: errors=%u warnings=%u (hidden-window test, NOT display-link validation)\n",
                errors, warnings);
    return errors ? 1 : 0;
}

#include "VideoTransitionStress.inl"

int main(int argc, char** argv) {
    if (argc == 2 && std::string(argv[1]) == "--transition-stress") return RunTransitionStress(false);
    if (argc == 2 && std::string(argv[1]) == "--transition-faults") return RunTransitionStress(true);
    if (argc == 2 && std::string(argv[1]) == "--presentation-debug") {
        return TestPresentationDebug();
    }
    if (argc == 2 && std::string(argv[1]) == "--presentation-recreate") {
        return TestPresentationRecreateReplay();
    }
    if (argc == 2 && std::string(argv[1]) == "--presentation-gpu") {
        return TestPresentationGpu();
    }
    TestPresentationPolicy();
    TestStartupWaitBoundary();
    TestSettingsCapabilityRefresh();
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
