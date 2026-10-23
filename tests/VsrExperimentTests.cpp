// Production renderer, synthetic NV12, real GPU. No capture/audio devices,
// visible windows, NVIDIA profile writes, user settings loads or saves.
#include "../src/main.cpp"
#undef fwprintf
#include "../src/audio/AsioOutput.cpp"
#include <limits>

#ifndef LLCV_VSR_EXPERIMENT
template<class Renderer>
constexpr bool HasPrivateTiming = requires(Renderer& renderer) { renderer.vsrTiming; };
static_assert(!HasPrivateTiming<DirectD3D11Renderer>,
              "production renderer must not carry private measurement histograms");
#endif

static void Require(bool value, const char* label) {
    if (!value) { std::fprintf(stderr, "FAIL: %s\n", label); std::exit(1); }
}
static void Check(HRESULT hr, const char* label) {
    if (FAILED(hr)) { std::fprintf(stderr, "FAIL: %s 0x%08X\n", label, static_cast<unsigned>(hr)); std::exit(1); }
}
static void TestPolicy() {
    using namespace llcv::vsr;
    Require(Eligible(0x10de, DXGI_FORMAT_NV12, false, 1280,720,1920,1080), "720p to 1080p eligible");
    const auto savedSettings = g_settings;
    const UINT savedVendor = g_vsrGpuVendor.load();
    for (UINT vendor : {0u, 0x8086u, 0x1002u, 0x1414u}) {
        g_vsrGpuVendor = vendor;
        g_settings.audioOnly = false;
        g_settings.vsrEnabled = false;
        g_vsrMode = Mode::Disabled;
        Require(HandleVsrTestKey(VK_F6, 0, 0) && !g_settings.vsrEnabled &&
            g_vsrMode.load() == Mode::Disabled &&
            g_transientHudContent.load() == TransientHudContent::VsrUnavailable,
            "Intel/AMD/unknown adapter cannot enable VSR through F6");
    }
    g_vsrGpuVendor = 0x10de;
    g_settings.videoPreset = VideoPreset::R1280x720;
    g_settings.capturePreset = VideoPreset::R1280x720;
    g_settings.videoFrameRate = 0;
    Require(CurrentVideoPreset().width == 1280 && CurrentVideoPreset().height == 720 &&
            RequestedVideoFrameRate() == 60, "720p capture preset defaults to 60 fps");
    g_settings.videoFrameRate = 120;
    Require(RequestedVideoFrameRate() == 120, "720p explicit FPS retained");
    g_settings.videoPreset = static_cast<VideoPreset>(-1);
    g_settings.displayMatchesCapture = false; // Validate a fixed preset, not the new follow-capture default.
    Require(CurrentVideoPreset().preset == VideoPreset::R1920x1080,
            "invalid preset fallback remains 1080p");
    // Resolve the capture/display plan once, before a graph starts. F6 must
    // never turn a 720p graph into a 4K graph or change window dimensions.
    for (bool matchCapture : {false, true}) for (bool enabled : {false, true})
    for (const auto& display : kVideoPresets) for (const auto& capture : kVideoPresets) {
        g_settings.videoPreset = display.preset;
        g_settings.capturePreset = capture.preset;
        g_settings.displayMatchesCapture = matchCapture;
        const auto& expectedDisplay = matchCapture ? capture : display;
        g_settings.vsrEnabled = enabled;
        g_settings.audioOnly = false;
        g_settings.pixelPerfect = true;
        g_settings.videoFrameRate = 0;
        LatchVideoResolutionPlan();
        Require(CurrentCapturePreset().preset == capture.preset, "capture selection independent of VSR");
        const SIZE initial = InitialClientPixelsForMonitor(nullptr);
        Require(initial.cx == expectedDisplay.width && initial.cy == expectedDisplay.height,
            "fixed and same-as-capture display selections are independent of VSR");
        Require(RequestedVideoFrameRate() == capture.framerate, "FPS default follows capture");
        Require(SourcePixelPerfect() == (expectedDisplay.preset == capture.preset), "split display never letterboxes as 1:1");
        g_vsrMode.store(Mode::On);
        for (int toggle = 0; toggle < 20; ++toggle) {
            HandleVsrTestKey(VK_F6, 0, 0);
            Require(CurrentCapturePreset().preset == capture.preset &&
                InitialClientPixelsForMonitor(nullptr).cx == expectedDisplay.width &&
                g_settings.videoPreset == display.preset,
                "F6 preserves latched capture and display across repeated toggles");
        }
    }
    g_resolutionPlanLatched = false;
    g_splitResolutionPlan = false;
    g_vsrMode.store(Mode::Disabled);
    g_settings = savedSettings;
    g_vsrGpuVendor = savedVendor;
    Require(Eligible(0x10de, DXGI_FORMAT_NV12, false, 1920,1080,3840,2160), "NV12 upscale eligible");
    Require(!Eligible(0x1002, DXGI_FORMAT_NV12, false, 1920,1080,3840,2160), "AMD untouched");
    Require(!Eligible(0x8086, DXGI_FORMAT_NV12, false, 1920,1080,3840,2160), "Intel untouched");
    Require(Eligible(0x10de, DXGI_FORMAT_P010, true, 1920,1080,3840,2160), "HDR10 upscale eligible");
    Require(Eligible(0x10de, DXGI_FORMAT_P010, true, 1920,1080,1920,1080), "HDR10 native eligible");
    Require(!Eligible(0x10de, DXGI_FORMAT_P010, true, 1920,1080,1919,2160), "HDR width downscale bypass");
    Require(!Eligible(0x10de, DXGI_FORMAT_P010, true, 1920,1080,3840,1079), "HDR height downscale bypass");
    Require(!Eligible(0x1002, DXGI_FORMAT_P010, true, 1920,1080,3840,2160), "HDR AMD bypass");
    Require(!Eligible(0x8086, DXGI_FORMAT_P010, true, 1920,1080,3840,2160), "HDR Intel bypass");
    Require(Eligible(0x10de, DXGI_FORMAT_YUY2, false, 1920,1080,3840,2160), "YUY2 upscale eligible");
    Require(Eligible(0x10de, DXGI_FORMAT_YUY2, false, 1920,1080,1920,1080), "YUY2 native eligible");
    Require(!Eligible(0x10de, DXGI_FORMAT_YUY2, true, 1920,1080,3840,2160), "YUY2 is SDR only");
    Require(Eligible(0x10de, DXGI_FORMAT_NV12, false, 1280,720,1280,720), "720p native eligible");
    Require(Eligible(0x10de, DXGI_FORMAT_NV12, false, 1920,1080,1920,1080), "1080p native eligible");
    Require(!Eligible(0x10de, DXGI_FORMAT_NV12, false, 1920,1080,1280,720), "downscale bypass");
    Require(Eligible(0x10de, DXGI_FORMAT_NV12, false, 1920,1080,3840,1080), "one unchanged axis eligible");
    Require(Eligible(0x10de, DXGI_FORMAT_NV12, false, 1920,1080,1920,2160), "other unchanged axis eligible");
    Require(!Eligible(0x10de, DXGI_FORMAT_NV12, false, 1920,1080,1919,2160), "width downscale bypass");
    Require(!Eligible(0x10de, DXGI_FORMAT_NV12, false, 1920,1080,3840,1079), "height downscale bypass");
    Require(!Eligible(0x10de, DXGI_FORMAT_NV12, false, 0,1080,3840,2160), "invalid size bypass");
    Require(!Eligible(0x10de, DXGI_FORMAT_NV12, false, 1920,0,3840,2160), "zero height bypass");
    Require(!Eligible(0x10de, DXGI_FORMAT_NV12, false, 1920,1080,0,1080), "zero output width bypass");
    Require(!Eligible(0x10de, DXGI_FORMAT_NV12, false, 1920,1080,1920,0), "zero output height bypass");
    Require(!Eligible(0x10de, DXGI_FORMAT_NV12, true, 1920,1080,1920,1080), "native HDR bypass");
    Require(!Eligible(0x10de, DXGI_FORMAT_P010, false, 1920,1080,1920,1080), "native P010 bypass");
    Require(!Eligible(0x1002, DXGI_FORMAT_NV12, false, 1920,1080,1920,1080), "native AMD bypass");
    Require(SetRequest(nullptr, nullptr, true) == E_POINTER, "null API guard");
    // Deterministic fault injection into the exact production retry policy.
    int blits = 0, disables = 0;
    State state = State::Requested;
    auto disable = [&] { ++disables; return S_OK; };
    auto failThenPass = [&] { return ++blits == 1 ? E_INVALIDARG : S_OK; };
    Require(ProcessWithFallback(state, failThenPass, disable) == S_OK &&
        state == State::Rejected && blits == 2 && disables == 1, "same-frame OFF fallback");
    Require(ProcessWithFallback(state, [&] { ++blits; return E_FAIL; }, disable) == E_FAIL &&
        blits == 3 && disables == 1, "rejected VSR never retries every frame");
    for (auto initial : {State::Untouched, State::Bypassed, State::Off, State::Rejected, State::Unknown}) {
        state = initial; blits = disables = 0;
        Require(ProcessWithFallback(state, [&] { ++blits; return E_FAIL; }, disable) == E_FAIL &&
            state == initial && blits == 1 && disables == 0, "ordinary failures propagate without VSR retry");
    }
    state = State::Requested; blits = disables = 0;
    Require(ProcessWithFallback(state, [&] { ++blits; return S_OK; }, disable) == S_OK &&
        state == State::Requested && blits == 1 && disables == 0, "successful frame adds no driver calls");
    for (HRESULT off : {E_FAIL, S_FALSE}) {
        state = State::Requested; blits = disables = 0;
        Require(FAILED(ProcessWithFallback(state, [&] { ++blits; return E_INVALIDARG; },
            [&] { ++disables; return off; })) && state == State::Unknown &&
            blits == 1 && disables == 1, "failed OFF cannot be claimed as safe fallback");
    }
    state = State::Requested; blits = disables = 0;
    Require(ProcessWithFallback(state, [&] { ++blits; return E_INVALIDARG; }, disable) == E_INVALIDARG &&
        state == State::Rejected && blits == 2, "failed fallback not hidden");
    for (HRESULT removed : {DXGI_ERROR_DEVICE_REMOVED, DXGI_ERROR_DEVICE_RESET, DXGI_ERROR_DEVICE_HUNG}) {
        state = State::Requested; blits = disables = 0;
        Require(ProcessWithFallback(state, [&] { ++blits; return removed; }, disable) == removed &&
            state == State::Requested && blits == 1 && disables == 0, "device loss remains a device recovery error");
    }
    Distribution d;
    d.Add(-1); d.Add(std::numeric_limits<double>::infinity());
    d.Add(std::numeric_limits<double>::quiet_NaN());
    Require(!d.count && d.Percentile(.99) == 0, "empty/invalid measurements");
    for (int i = 0; i < 100; ++i) d.Add(1);
    d.Add(150);
    Require(d.count == 101 && d.maximum == 150 && d.Percentile(.99) < 1.2 &&
        d.Percentile(1) == 150, "histogram percentile and overflow");
    Timing timing;
    timing.Begin(nullptr); timing.End(nullptr, false); timing.AddCaptureToPresent(2);
    Require(!timing.Snapshot().captureToPresent.count, "disabled timing inert");
}

static LRESULT CALLBACK SettingsControllerTestProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    // Exercise the real controller and keyboard mappings without WM_CREATE's
    // hardware enumeration or background workers. The fixture supplies controls.
    if (message == WM_CREATE) return 0;
    return SettingsWndProc(hwnd,message,wParam,lParam);
}
static void TestExclusiveScanLifecycle() {
    const auto savedSettings = g_settings;
    const bool savedSuppress = g_suppressSettingsSave;
    g_suppressSettingsSave = true; // No user INI writes or real audio devices.
    WNDCLASSW wc{};
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpfnWndProc = SettingsControllerTestProc;
    wc.lpszClassName = L"LLCV_EXCLUSIVE_LIFECYCLE_TEST";
    Require(RegisterClassW(&wc) != 0, "exclusive lifecycle class");
    // Normal completion, partial cancellation, and unread results after close.
    for (int scenario = 0; scenario < 3; ++scenario) {
        g_settings = {};
        g_settings.volumePercent = 73;
        SettingsDialogState state;
        state.activeTab = SettingsTab::Audio;
        state.probeReady = true;
        state.appliedSharedEndpoint = L"fake-endpoint";
        state.audioEndpoints = {{L"fixture-a", L"Fixture A", true},
                                {L"fixture-b", L"Fixture B", false}};
        state.exclusiveEndpointResults.resize(2);
        for (auto& result : state.exclusiveEndpointResults)
            result.state = ExclusiveEndpointState::Testing;
        state.exclusiveScanRunning = true;
        HWND hwnd = CreateWindowW(wc.lpszClassName, L"Exclusive lifecycle", WS_POPUP,
            -32000, -32000, 1000, 650, nullptr, nullptr, wc.hInstance, &state);
        Require(hwnd != nullptr, "exclusive lifecycle window");
        const llcv::settings_ui::SettingsControlInitialValues initial{
            g_settings, false, true, kAppVersionLabel, VideoPreset::R1920x1080,
            {}, {}, kVideoPresets, kPcmQueueOptionsMs, {}};
        const llcv::settings_ui::SettingsControlPopulation population{
            &state,
            [](void* s) { PopulateAudioOutputCombo(static_cast<SettingsDialogState*>(s)); },
            [](void* s) { PopulateSettingsBufferCombo(static_cast<SettingsDialogState*>(s)); },
            [](void*) {}};
        llcv::settings_ui::CreateSettingsDialogControls(&state, hwnd, wc.hInstance, initial, population);
        SendMessageW(state.audioCombo, CB_SETCURSEL, 1, 0);
        PopulateAudioOutputCombo(&state);
        state.selectedBufferMs = 5;
        ExclusiveEndpointProbeResult result{};
        result.probe.compatible = true;
        result.probe.requestedFrames = 480; // Verified minimum 10 ms.
        QueueExclusiveEndpointProbeResult(&state, result);
        if (scenario == 2) {
            // Closing the HWND discards notifications, not completed diagnostics.
            FinishSettingsDialog(hwnd, &state, false);
            CompleteExclusiveEndpointScan(&state);
        } else {
            SendMessageW(hwnd, WM_EXCLUSIVE_ENDPOINT_PROBE_COMPLETE, 0, 0);
            Require(state.selectedBufferMs == 10, "too-small buffer raised to verified minimum");
            Require(g_settings.exclusiveEndpointCache.size() == 1,
                    "completed endpoint persists before whole scan finishes");
            state.selectedBufferMs = 30;
            PopulateSettingsBufferCombo(&state);
            if (scenario == 0) {
                result.endpointIndex = 1;
                QueueExclusiveEndpointProbeResult(&state, result);
                SendMessageW(hwnd, WM_EXCLUSIVE_ENDPOINT_PROBE_COMPLETE, 0, 0);
                Require(state.selectedBufferMs == 30,
                        "other endpoint completion preserves user buffer");
                // Duplicate delivery cannot inflate progress or change choices.
                QueueExclusiveEndpointProbeResult(&state, result);
                SendMessageW(hwnd, WM_EXCLUSIVE_ENDPOINT_PROBE_COMPLETE, 0, 0);
                Require(state.exclusiveScanCompleted == 2 && state.selectedBufferMs == 30,
                        "duplicate result is idempotent");
            } else {
                state.exclusiveProbeStop = true;
            }
            SendMessageW(hwnd, WM_EXCLUSIVE_SCAN_COMPLETE, 0, 0);
            Require(state.selectedBufferMs == 30, "scan completion preserves user buffer");
            DestroyWindow(hwnd);
        }
        const size_t expected = scenario == 0 ? 2 : 1;
        Require(state.exclusiveScanCompleted == expected &&
                g_settings.exclusiveEndpointCache.size() == expected &&
                !state.exclusiveScanRunning, "only finished endpoints persist on completion/close");
        Require(scenario == 0 ||
                state.exclusiveEndpointResults[1].state == ExclusiveEndpointState::Unknown,
                "cancelled untested endpoint remains unknown");
        Require(HasVerifiedExclusiveEndpoint(L"fixture-a", 30) &&
                !HasVerifiedExclusiveEndpoint(L"fixture-a", 5),
                "retained diagnostic remains usable without rescanning");
        Require(g_settings.volumePercent == 73, "diagnostic saves preserve settings");
        CompleteExclusiveEndpointScan(&state);
        Require(g_settings.exclusiveEndpointCache.size() == expected,
                "completion and shutdown can safely share finalization");
    }
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    g_settings = savedSettings;
    g_suppressSettingsSave = savedSuppress;
    std::puts("Exclusive lifecycle: buffer preservation, partial save, close and duplicates passed.");
}

static void TestConnectedMjpegColor() {
    using namespace llcv::video;
    using namespace llcv::video_color;
    VIDEOINFOHEADER2 info{};
    info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth=640;info.bmiHeader.biHeight=480;
    info.bmiHeader.biCompression=MAKEFOURCC('M','J','P','G');
    info.bmiHeader.biSizeImage=640*480*3;info.AvgTimePerFrame=166667;
    AM_MEDIA_TYPE type{};
    type.majortype=MEDIATYPE_Video;type.subtype=MEDIASUBTYPE_MJPG;
    type.formattype=FORMAT_VideoInfo2;type.cbFormat=sizeof(info);
    type.pbFormat=reinterpret_cast<BYTE*>(&info);
    for(unsigned scenario=0;scenario<4;++scenario) {
        DirectShowColorMetadata effective{};
        if(scenario!=0) {
            effective.present=true;effective.transferMatrix=2;effective.nominalRange=1;
        }
        // Connection-only full declaration, override a selected hint, partial
        // declaration, or no declaration (preserve selected/capability hints).
        info.dwControlFlags=scenario==3 ? 0 : 0x81u | (2u<<12) | (scenario==2?0u:1u<<15);
        VideoSampleFormat format{};
        Require(ReadConnectedVideoSampleFormat(&type,60,effective,format),
            "production final connected format accepts MJPEG metadata");
        const unsigned matrix=scenario<2?1u:2u, range=scenario==3?1u:2u;
        Require(effective.present && effective.transferMatrix==matrix && effective.nominalRange==range &&
            format.color.transferMatrix==matrix && format.color.nominalRange==range,
            "playback and recording signature retain identical final metadata");
        Require(MatchesVideoSampleFormat(&type,format),
            "identical connected MJPEG announcement is not rejected as a color change");
        auto changed=info;changed.dwControlFlags=0x81u|((range==1?2u:1u)<<12)|(matrix<<15);
        auto changedType=type;changedType.pbFormat=reinterpret_cast<BYTE*>(&changed);
        Require(!MatchesVideoSampleFormat(&changedType,format),
            "real MJPEG range change is still rejected by the dynamic-type guard");
        for(auto overrideMode:{MatrixOverride::Auto,MatrixOverride::Bt709,MatrixOverride::Bt601})
        for(auto rangeOverride:{RangeOverride::Auto,RangeOverride::Limited,RangeOverride::Full}) {
            MjpegDecoder playback, recording;
            Check(playback.initialize(640,480,60,&type,&effective,overrideMode,nullptr,rangeOverride),
                "playback MJPEG decoder uses effective connected color");
            Check(recording.initialize(640,480,60,&type,&format.color,overrideMode,nullptr,rangeOverride),
                "recording MJPEG decoder uses the same connected color");
            Require(playback.colorConfiguration()==recording.colorConfiguration(),
                "automatic and manually overridden MJPEG playback/recording colors agree");
        }
    }
    std::puts("MJPEG connected color: connection-only/override/partial/omitted hints and decoder parity passed.");
}

#ifdef LLCV_RECORDING
static LRESULT CALLBACK RecordingShortcutFixtureProc(HWND hwnd,UINT message,WPARAM w,LPARAM l) {
    if(message==WM_COMMAND && LOWORD(w)==llcv::settings_ui::kRecordingToggleCommand) {
        ++*reinterpret_cast<unsigned*>(GetWindowLongPtrW(hwnd,GWLP_USERDATA));return 0;
    }
    return DefWindowProcW(hwnd,message,w,l);
}
static void TestRecordingSettingsShortcuts() {
    const auto saved=g_settings.shortcuts;
    WNDCLASSW wc{};wc.hInstance=GetModuleHandleW(nullptr);
    wc.lpfnWndProc=RecordingShortcutFixtureProc;wc.lpszClassName=L"LLCV_RECORDING_SHORTCUT_TEST";
    Require(RegisterClassW(&wc)!=0,"recording shortcut fixture class");
    HWND dialog=CreateWindowW(wc.lpszClassName,L"",WS_POPUP,-32000,-32000,100,100,
        nullptr,nullptr,wc.hInstance,nullptr);
    HWND child=CreateWindowW(L"EDIT",L"",WS_CHILD,0,0,50,20,dialog,nullptr,wc.hInstance,nullptr);
    Require(dialog&&child,"recording shortcut fixture windows");
    unsigned toggles=0;SetWindowLongPtrW(dialog,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(&toggles));
    for(unsigned binding:{unsigned(VK_SPACE),unsigned(VK_RETURN),unsigned(VK_TAB),unsigned(VK_ESCAPE),
        unsigned(VK_LEFT),unsigned(VK_RIGHT),unsigned(VK_UP),unsigned(VK_DOWN),unsigned(VK_HOME),
        unsigned(VK_END),unsigned(VK_PRIOR),unsigned(VK_NEXT),unsigned(VK_BACK),unsigned(VK_DELETE),
        unsigned(VK_F4),unsigned('A'),keys::Ctrl|'A',keys::Ctrl|'V',keys::Alt|'A',unsigned(VK_NUMPAD2)}) {
        g_settings.shortcuts[static_cast<size_t>(keys::Action::Recording)]=binding;
        MSG key{};key.hwnd=child;key.message=WM_KEYDOWN;key.wParam=binding&0xff;
        Require(!ProcessSettingsRecordingShortcut(dialog,key,binding&0x700) && toggles==0,
            "navigation/editing/native F4 cannot start or stop recording from settings");
    }
    for(unsigned binding:{unsigned(VK_F9),keys::Ctrl|VK_F10}) {
        g_settings.shortcuts[static_cast<size_t>(keys::Action::Recording)]=binding;
        MSG key{};key.hwnd=child;key.message=WM_KEYDOWN;key.wParam=binding&0xff;
        const unsigned before=toggles;
        Require(ProcessSettingsRecordingShortcut(dialog,key,binding&0x700) && toggles==before+1,
            "default/custom function recording key dispatches exactly once from settings child");
        key.lParam=LPARAM{1}<<30;
        Require(ProcessSettingsRecordingShortcut(dialog,key,binding&0x700) && toggles==before+1,
            "recording key repeat is consumed without a second toggle");
        key.message=WM_KEYUP;
        Require(!ProcessSettingsRecordingShortcut(dialog,key,binding&0x700),"key release never toggles recording");
        key.message=WM_KEYDOWN;key.hwnd=nullptr;
        Require(!ProcessSettingsRecordingShortcut(dialog,key,binding&0x700),"other windows are not settings shortcuts");
    }
    DestroyWindow(dialog);UnregisterClassW(wc.lpszClassName,wc.hInstance);
    g_settings.shortcuts=saved;
    std::puts("Recording settings shortcuts: native navigation/editing, F4, F9/custom function key and repeats passed.");
}
#endif

static void TestSettingsController() {
    TestConnectedMjpegColor();
#ifdef LLCV_RECORDING
    TestRecordingSettingsShortcuts();
#endif
    Require(!SuppressSettingsWritesForRun(nullptr) &&
            !SuppressSettingsWritesForRun(L"") &&
            !SuppressSettingsWritesForRun(L"--settings-tab 2"),
            "ordinary recording-enabled builds retain settings persistence");
    for (const auto* command : {L"--smoke-test", L"--smoke-test-audio-only",
                                L"--smoke-test-asio driver", L"--exclusive-probe-all"})
        Require(SuppressSettingsWritesForRun(command),
                "diagnostic runs suppress both migrations and settings saves");
    const auto savedSettings = g_settings;
    const bool savedSuppress = g_suppressSettingsSave;
    const HWND previousForeground = GetForegroundWindow();
    g_suppressSettingsSave = true;
    g_settings.captureDeviceId = L"controller-test-sentinel";
    g_settings.videoPreset = VideoPreset::R2560x1440;
    g_settings.displayMatchesCapture = true;
    g_settings.audioOnly = false;
    g_settings.vsrEnabled = true;
    g_settings.volumePercent = 73;
    g_settings.pcmQueueTargetMs = 25;
    g_settings.sdrRangeOverride = llcv::video_color::RangeOverride::Auto;
    g_settings.legacyMjpegRangeOverride = llcv::video_color::RangeOverride::Full;
    g_settings.mjpegMatrixOverride = llcv::video_color::MatrixOverride::Bt601;
    g_settings.shortcuts=keys::Defaults;
#ifdef LLCV_RECORDING
    {
        const auto previousHud=g_transientHudContent.load();
        const auto previousUntil=g_volumeHudUntilMs.load();
        const auto options=llcv::recording::sessionOptions;
        wchar_t temp[MAX_PATH]{};Require(GetTempPathW(MAX_PATH,temp)!=0,"recording test temp folder");
        llcv::recording::sessionOptions.folder=temp;
        g_transientHudContent=TransientHudContent::Volume;
        Require(HandleViewerShortcut(nullptr,VK_F9,LPARAM{1}<<30,0) &&
            g_transientHudContent==TransientHudContent::Volume,"recording repeat does not toggle");
        g_settings.audioOnly=true;
        Require(HandleViewerShortcut(nullptr,VK_F9,0,0) &&
            g_transientHudContent==TransientHudContent::Volume,"audio-only recording shortcut is inert");
        g_settings.audioOnly=false;
        Require(!HandleViewerShortcut(nullptr,VK_F9,0,keys::Ctrl),"extra modifier does not record");
        Require(HandleViewerShortcut(nullptr,VK_F9,0,0) &&
            llcv::recording::recorder.GetSnapshot().phase==llcv::recording::Phase::Failed &&
            !llcv::recording::recorder.Busy(),"invalid capture gets failure HUD without starting writer");
        g_settings.shortcuts[static_cast<size_t>(keys::Action::Recording)]=keys::Ctrl|'R';
        g_transientHudContent=TransientHudContent::Volume;
        Require(!HandleViewerShortcut(nullptr,VK_F9,0,0),"old recording key no longer acts");
        Require(HandleViewerShortcut(nullptr,'R',0,keys::Ctrl) &&
            llcv::recording::recorder.GetSnapshot().phase==llcv::recording::Phase::Failed,"custom recording combination routes");
        g_settings.shortcuts=keys::Defaults;
        llcv::recording::sessionOptions=options;
        g_transientHudContent=previousHud;g_volumeHudUntilMs=previousUntil;
    }
#endif
    g_settings.shortcuts[0]=VK_F10;
    auto unchanged = [&] {
        Require(g_settings.captureDeviceId == L"controller-test-sentinel" &&
            g_settings.videoPreset == VideoPreset::R2560x1440 && g_settings.displayMatchesCapture && !g_settings.audioOnly &&
            g_settings.vsrEnabled && g_settings.volumePercent == 73 && g_settings.pcmQueueTargetMs == 25 &&
            g_settings.shortcuts[0]==VK_F10 &&
            g_settings.sdrRangeOverride == llcv::video_color::RangeOverride::Auto &&
            g_settings.legacyMjpegRangeOverride == llcv::video_color::RangeOverride::Full &&
            g_settings.mjpegMatrixOverride == llcv::video_color::MatrixOverride::Bt601,
            "blocked/cancelled controller action preserves settings");
    };
    WNDCLASSW wc{};
    wc.hInstance = GetModuleHandleW(nullptr); wc.lpfnWndProc = SettingsControllerTestProc;
    wc.lpszClassName = L"LLCV_SETTINGS_CONTROLLER_TEST";
    Require(RegisterClassW(&wc) != 0, "controller test class");
#ifdef LLCV_RECORDING
    {
        SettingsDialogState state;
        for(auto tab:{SettingsTab::VideoWindow,SettingsTab::Audio,SettingsTab::Window,
                      SettingsTab::GuideDiagnostics,SettingsTab::Updates}){
            state.activeTab=tab;RequestRecordingEncoders(&state,nullptr);
            Require(!state.recordingProbe,"non-recording pages never launch encoder discovery");
        }
        state.activeTab=SettingsTab::Recording;state.recordingProbeStop=true;
        RequestRecordingEncoders(&state,nullptr);
        Require(!state.recordingProbe,"closing prevents a new encoder probe");
        state.recordingProbeStop=false;
        HWND hwnd=CreateWindowW(wc.lpszClassName,L"Probe close fixture",WS_POPUP,
            -32000,-32000,100,100,nullptr,nullptr,wc.hInstance,&state);
        Require(hwnd!=nullptr,"encoder probe close fixture");
        std::atomic<bool> started{false},cancelled{false};
        state.recordingProbe=std::make_unique<llcv::settings_ui::SettingsModeCache<llcv::recording::EncoderProbeResult>>(
            [&](const auto&){
                started=true;
                const auto deadline=GetTickCount64()+2000;
                while(!state.recordingProbeStop && GetTickCount64()<deadline)Sleep(1);
                cancelled=state.recordingProbeStop.load();
                return llcv::recording::EncoderProbeResult{};
            },[]{});
        state.recordingProbe->Request({});
        const auto deadline=GetTickCount64()+2000;
        while(!started && GetTickCount64()<deadline)Sleep(1);
        Require(started,"fixture probe started");
        FinishSettingsDialog(hwnd,&state,false);
        Require(!IsWindow(hwnd)&&state.recordingProbeStop,"settings close hides window and cancels pending encoder work");
        state.recordingProbe.reset();
        Require(cancelled,"closing does not join the probe before cancellation reaches it");
    }
#endif
    auto create = [&](SettingsDialogState& state) {
        state.activeTab = SettingsTab::VideoWindow;
#ifdef LLCV_RECORDING
        state.recordingProbePending=false; // This fixture supplies capabilities; never open a real encoder.
#endif
        // Offscreen native controls permit real focus/dropdown behavior. No
        // capture/audio device, settings file, update request or worker opens.
        HWND hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_CONTROLPARENT | WS_EX_NOACTIVATE,
            wc.lpszClassName,L"Offscreen settings controller",WS_POPUP | WS_VISIBLE | WS_CLIPCHILDREN,
            -32000,-32000,950,650,nullptr,nullptr,wc.hInstance,&state);
        Require(hwnd != nullptr, "controller fixture window");
        auto child = [&](const wchar_t* kind, const wchar_t* text, DWORD style, int id) {
            HWND result = CreateWindowW(kind,text,WS_CHILD | WS_VISIBLE | WS_TABSTOP | style,
                10,10,300,120,hwnd,reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),wc.hInstance,nullptr);
            Require(result != nullptr, "controller fixture child");
            return result;
        };
        state.tabControl = child(L"LISTBOX",L"",LBS_NOTIFY | LBS_HASSTRINGS | LBS_OWNERDRAWFIXED,IDC_SETTINGS_TAB);
        for (int index=0;index<llcv::settings_ui::kSettingsNavigationCount;++index)
            SendMessageW(state.tabControl,LB_ADDSTRING,0,reinterpret_cast<LPARAM>(L"Test page"));
        state.audioCombo = child(L"COMBOBOX",L"",CBS_DROPDOWNLIST,IDC_SETTINGS_AUDIO);
        for (const auto* label : {L"Shared",L"Exclusive"})
            SendMessageW(state.audioCombo,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(label));
        SendMessageW(state.audioCombo,CB_SETCURSEL,0,0);
        state.audioOnlyCheck = child(L"BUTTON",L"Audio only",BS_AUTOCHECKBOX,IDC_SETTINGS_AUDIO_ONLY);
        state.pixelFormatCombo = child(L"COMBOBOX",L"",CBS_DROPDOWNLIST,IDC_SETTINGS_PIXEL_FORMAT);
        const auto nv12 = SendMessageW(state.pixelFormatCombo,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(L"NV12"));
        SendMessageW(state.pixelFormatCombo,CB_SETITEMDATA,nv12,static_cast<LPARAM>(VideoPixelFormat::Nv12));
        SendMessageW(state.pixelFormatCombo,CB_SETCURSEL,nv12,0);
        state.sdrRangeOverride = g_settings.sdrRangeOverride;
        state.legacyMjpegRangeOverride = g_settings.legacyMjpegRangeOverride;
        state.sdrRangeCombo = child(L"COMBOBOX",L"",CBS_DROPDOWNLIST,IDC_SETTINGS_SDR_RANGE);
        state.mjpegColorCombo = child(L"COMBOBOX",L"",CBS_DROPDOWNLIST,IDC_SETTINGS_MJPEG_COLOR);
        for (HWND combo : {state.sdrRangeCombo, state.mjpegColorCombo}) {
            for (int value = 0; value < 3; ++value) {
                const auto row = SendMessageW(combo,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(L"Color choice"));
                SendMessageW(combo,CB_SETITEMDATA,row,value);
            }
        }
        SendMessageW(state.mjpegColorCombo,CB_SETCURSEL,
            static_cast<WPARAM>(g_settings.mjpegMatrixOverride),0);
        state.frameRateCombo = child(L"COMBOBOX",L"",CBS_DROPDOWNLIST,IDC_SETTINGS_FRAME_RATE);
        SendMessageW(state.frameRateCombo,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(L"60 fps"));
        state.startButton = child(L"BUTTON",L"Start",BS_DEFPUSHBUTTON,IDC_SETTINGS_START);
        state.cancelButton = child(L"BUTTON",L"Cancel",BS_PUSHBUTTON,IDC_SETTINGS_CANCEL);
        Require(GetDlgCtrlID(state.startButton) == 2004 && GetDlgItem(hwnd,IDOK) == nullptr,
            "fixture retains production non-IDOK Start identifier");
        LayoutSettingsControls(&state,96);
        UpdateAdvancedControlVisibility(&state);
        Require(state.theme.Attach(hwnd,&state), "controller fixture theme");
        return hwnd;
    };
    auto key = [](HWND owner, HWND target, WPARAM value) {
        SetFocus(target);
        Require(GetFocus() == target, "controller keyboard fixture focus");
        MSG message{}; message.hwnd = target; message.message = WM_KEYDOWN;
        message.wParam = value; message.lParam = 1;
        if (!IsDialogMessageW(owner,&message)) DispatchMessageW(&message);
    };
    {
        SettingsDialogState state;
        HWND hwnd = create(state);
        state.shortcutBindings=g_settings.shortcuts;
        SendMessageW(hwnd,WM_COMMAND,MAKEWPARAM(IDC_SETTINGS_SHORTCUT_RESET,BN_CLICKED),0);
        Require(state.shortcutBindings==keys::Defaults && g_settings.shortcuts[0]==VK_F10,
            "reset affects only the working shortcut copy, not saved settings");
        state.shortcutBindings[0]=keys::Ctrl|'H';
        state.pixelFormats.push_back({VideoPixelFormat::Nv12,60});
        UpdateAdvancedControlVisibility(&state);
        SendMessageW(state.sdrRangeCombo,CB_SETCURSEL,1,0);
        SendMessageW(hwnd,WM_COMMAND,MAKEWPARAM(IDC_SETTINGS_SDR_RANGE,CBN_SELCHANGE),
            reinterpret_cast<LPARAM>(state.sdrRangeCombo));
        Require(state.sdrRangeOverride == llcv::video_color::RangeOverride::Limited &&
            state.legacyMjpegRangeOverride == llcv::video_color::RangeOverride::Auto,
            "explicit SDR range edits replace only pending legacy MJPEG preferences");
        Require(SettingsCanStart(&state), "valid mocked capture mode can start");
        EnableWindow(state.startButton,FALSE);
        SendMessageW(hwnd,WM_COMMAND,MAKEWPARAM(IDOK,BN_CLICKED),0);
        Require(IsWindow(hwnd) && !state.accepted && !state.exclusiveProbeStop.load(),
            "IDOK cannot activate disabled Start or stop workers");
        key(hwnd,state.pixelFormatCombo,VK_RETURN);
        Require(IsWindow(hwnd) && !state.accepted, "Enter obeys disabled Start with production IDs");
        unchanged();
        state.pixelFormats.clear();
        EnableWindow(state.startButton,TRUE); // stale UI state must not bypass the data guard
        SendMessageW(hwnd,WM_COMMAND,MAKEWPARAM(IDOK,BN_CLICKED),0);
        Require(IsWindow(hwnd) && !state.accepted, "IDOK rejects enabled button with invalid capture capabilities");
        unchanged();
        for (int index = 0; index < llcv::settings_ui::kSettingsNavigationCount; ++index) {
            SendMessageW(state.tabControl,LB_SETCURSEL,index,0);
            SendMessageW(hwnd,WM_COMMAND,MAKEWPARAM(IDC_SETTINGS_TAB,LBN_SELCHANGE),
                reinterpret_cast<LPARAM>(state.tabControl));
            Require(state.activeTab == llcv::settings_ui::SettingsTabFromNavigationIndex(index) &&
                !IsWindowEnabled(state.startButton), "navigation preserves unsupported-mode Start gate");
            if (state.activeTab == SettingsTab::VideoWindow)
                Require(!IsWindowEnabled(state.pixelFormatCombo) && !IsWindowEnabled(state.frameRateCombo),
                    "navigation preserves unsupported format/FPS gates");
        }
        SendMessageW(state.audioOnlyCheck,BM_SETCHECK,BST_CHECKED,0);
        UpdateAdvancedControlVisibility(&state);
        Require(SettingsCanStart(&state) && IsWindowEnabled(state.startButton),
            "audio-only Shared can start without video capabilities");
        SendMessageW(state.audioCombo,CB_SETCURSEL,1,0);
        UpdateAdvancedControlVisibility(&state);
        Require(!SettingsCanStart(&state) && !IsWindowEnabled(state.startButton),
            "audio-only still requires Exclusive verification");
        SendMessageW(state.audioCombo,CB_SETCURSEL,0,0);
        state.activeTab = SettingsTab::VideoWindow;
        state.pixelFormats.push_back({VideoPixelFormat::Nv12,60});
        UpdateAdvancedControlVisibility(&state);
        SetFocus(state.pixelFormatCombo);
        SendMessageW(state.pixelFormatCombo,CB_SHOWDROPDOWN,TRUE,0);
        Require(SendMessageW(state.pixelFormatCombo,CB_GETDROPPEDSTATE,0,0) != 0, "native dropdown opened");
        key(hwnd,state.pixelFormatCombo,VK_ESCAPE);
        Require(IsWindow(hwnd) && !state.accepted &&
            SendMessageW(state.pixelFormatCombo,CB_GETDROPPEDSTATE,0,0) == 0,
            "first Escape closes dropdown without cancelling settings");
        key(hwnd,state.pixelFormatCombo,VK_ESCAPE);
        Require(!IsWindow(hwnd) && !state.accepted, "second Escape cancels settings through IDCANCEL");
        unchanged();
    }
    {
        SettingsDialogState state;
        HWND hwnd = create(state);
        state.shortcutBindings[0]=VK_F10;
        key(hwnd,state.cancelButton,VK_RETURN);
        Require(!IsWindow(hwnd) && !state.accepted, "Enter on production Cancel cancels, not starts");
        unchanged();
    }
    {
        SettingsDialogState state;
        HWND hwnd=create(state);
        state.shortcutBindings[0]=keys::Ctrl|'H';
        state.pixelFormats.push_back({VideoPixelFormat::Nv12,60});
        UpdateAdvancedControlVisibility(&state);
        SendMessageW(state.sdrRangeCombo,CB_SETCURSEL,2,0);
        SendMessageW(hwnd,WM_COMMAND,MAKEWPARAM(IDC_SETTINGS_SDR_RANGE,CBN_SELCHANGE),
            reinterpret_cast<LPARAM>(state.sdrRangeCombo));
        SendMessageW(state.mjpegColorCombo,CB_SETCURSEL,1,0);
        FinishSettingsDialog(hwnd,&state,true);
        Require(!IsWindow(hwnd) && state.accepted &&
            g_settings.shortcuts==state.shortcutBindings &&
            g_settings.sdrRangeOverride == llcv::video_color::RangeOverride::Full &&
            g_settings.legacyMjpegRangeOverride == llcv::video_color::RangeOverride::Auto &&
            g_settings.mjpegMatrixOverride == llcv::video_color::MatrixOverride::Bt709,
            "Start commits the edited shortcut, common SDR range and separate MJPEG matrix snapshot");
    }
    UnregisterClassW(wc.lpszClassName,wc.hInstance);
    if (IsWindow(previousForeground)) SetForegroundWindow(previousForeground);
    g_settings = savedSettings;
    g_suppressSettingsSave = savedSuppress;
    std::puts("PASS: real settings controller IDs, disabled/invalid Start guards, navigation, audio-only and native keyboard routing.");
}

struct AudioUiMessages {
    unsigned enables = 0, textWrites = 0, resets = 0, paints = 0, directDraws = 0;
};
static LRESULT CALLBACK CountAudioUiMessages(HWND hwnd, UINT message, WPARAM wParam,
    LPARAM lParam, UINT_PTR, DWORD_PTR reference) {
    auto& count = *reinterpret_cast<AudioUiMessages*>(reference);
    if (message == WM_ENABLE && wParam) ++count.enables;
    if (message == WM_SETTEXT) ++count.textWrites;
    if (message == CB_RESETCONTENT) ++count.resets;
    if (message == WM_PAINT) ++count.paints;
    // Native STATIC can draw straight to a DC during WM_SETTEXT/WM_ENABLE,
    // without sending WM_PAINT. Catch visible intermediate text draws too.
    if (message == WM_CTLCOLORSTATIC && IsWindowVisible(hwnd)) ++count.directDraws;
    return DefSubclassProc(hwnd, message, wParam, lParam);
}
static void TestSettingsAudioTransitions() {
    const auto savedSettings = g_settings;
    const bool savedSuppress = g_suppressSettingsSave;
    g_suppressSettingsSave = true;
    WNDCLASSW wc{};
    wc.hInstance = GetModuleHandleW(nullptr); wc.lpfnWndProc = SettingsControllerTestProc;
    wc.lpszClassName = L"LLCV_SETTINGS_AUDIO_TRANSITION_TEST";
    Require(RegisterClassW(&wc) != 0, "audio transition test class");
    unsigned transitions = 0;
    for (bool light : {false, true}) for (UINT dpi : {96u, 144u}) {
        SettingsDialogState state;
        state.activeTab = SettingsTab::Audio;
        state.asioAvailable = true; // UI-only; no real driver is enumerated
        state.probeReady = true;
        state.audioEndpoints.resize(1);
        state.audioEndpoints[0].id = L"fake-endpoint";
        state.audioEndpoints[0].name = L"Fixture speakers";
        state.audioEndpoints[0].isDefault = true;
        state.exclusiveEndpointResults.resize(1);
        state.exclusiveEndpointResults[0].state = ExclusiveEndpointState::Testing;
        // Simulate a worker in progress: no hardware enumeration or opening.
        state.exclusiveScanRunning = true;
        HWND hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
            wc.lpszClassName, L"Audio transition fixture", WS_POPUP | WS_CLIPCHILDREN,
            -32000, -32000, llcv::settings_ui::SettingsPixels(1000,dpi),
            llcv::settings_ui::SettingsPixels(650,dpi),
            nullptr, nullptr, wc.hInstance, &state);
        Require(hwnd != nullptr, "audio transition fixture");
        AppSettings initialSettings{};
        initialSettings.consoleSurround51 = true;
        const llcv::settings_ui::SettingsControlInitialValues initial{
            initialSettings, false, true, kAppVersionLabel, VideoPreset::R1920x1080,
            {}, {}, kVideoPresets, kPcmQueueOptionsMs, {}};
        const llcv::settings_ui::SettingsControlPopulation population{
            &state,
            [](void* s) { PopulateAudioOutputCombo(static_cast<SettingsDialogState*>(s)); },
            [](void* s) { PopulateSettingsBufferCombo(static_cast<SettingsDialogState*>(s)); },
            [](void*) {}};
        llcv::settings_ui::CreateSettingsDialogControls(&state, hwnd, wc.hInstance, initial, population);
        ApplySettingsFont(&state, hwnd, dpi);
        LayoutSettingsControls(&state, dpi);
        SendMessageW(state.themeCombo, CB_SETCURSEL, light ? 1 : 0, 0);
        UpdateAdvancedControlVisibility(&state);
        Require(state.theme.Attach(hwnd, &state), "audio transition theme");
        state.theme.RefreshControls(dpi);
        ShowWindow(hwnd, SW_SHOWNOACTIVATE);
        RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW);
        AudioUiMessages button, hint, status, output, buffer, owner;
        for (auto pair : {std::pair{state.exclusiveTestButton,&button},
                          std::pair{state.surround51Hint,&hint}, std::pair{state.audioStatus,&status},
                          std::pair{state.audioOutputCombo,&output}, std::pair{state.bufferCombo,&buffer},
                          std::pair{hwnd,&owner}})
            Require(SetWindowSubclass(pair.first, CountAudioUiMessages, 99,
                reinterpret_cast<DWORD_PTR>(pair.second)) != FALSE, "audio message observer");
        auto selectMode = [&](int mode) {
            SendMessageW(state.audioCombo, CB_SETCURSEL, mode, 0);
            SendMessageW(hwnd, WM_COMMAND, MAKEWPARAM(IDC_SETTINGS_AUDIO,CBN_SELCHANGE),
                reinterpret_cast<LPARAM>(state.audioCombo));
        };
        for (int i = 0; i < 100; ++i) {
            button = {}; hint = {}; status = {}; owner = {};
            const bool exclusive = i % 2 == 0;
            selectMode(exclusive ? 1 : 0);
            Require(button.enables == 0, "running scan button never transiently enables during mode change");
            Require(IsWindowEnabled(state.surround51Check) == !exclusive &&
                    IsWindowEnabled(state.surround51Hint) == !exclusive,
                    "surround state follows final output mode");
            Require(SendMessageW(state.surround51Check,BM_GETCHECK,0,0) == BST_CHECKED,
                    "mode changes retain surround preference");
            Require(status.textWrites <= 1, "one final status per mode change");
            Require(owner.directDraws == 0, "no visible direct text draws mid-transition");
            Require(!state.exclusiveProbeThread.joinable() && state.exclusiveScanRunning,
                    "repeated selection reuses pending scan");
            RedrawWindow(hwnd, nullptr, nullptr, RDW_ALLCHILDREN | RDW_UPDATENOW);
            Require(button.paints <= 1 && hint.paints <= 1 && status.paints <= 1,
                    "mode transition coalesces to one final paint per affected control");
            ++transitions;
        }
        selectMode(1);
        button = {}; output = {}; buffer = {}; status = {};
        for (int i = 0; i < 25; ++i) {
            UpdateAdvancedControlVisibility(&state);
            UpdateExclusiveProbeControl(&state);
        }
        Require(button.enables == 0 && button.textWrites == 0,
                "unchanged busy state never enables or rewrites the scan button");
        SendMessageW(hwnd, WM_AUDIOCLIENT3_PROBE_COMPLETE, 0, 0);
        Require(output.resets == 0 && buffer.resets == 0 && status.textWrites == 0,
                "late Shared completion leaves Exclusive UI unchanged");
        selectMode(0);
        // An Exclusive result arriving after switching back must not rebuild
        // Shared fields or silently replace the user's buffer selection.
        output = {}; buffer = {}; status = {};
        const int oldBuffer = state.selectedBufferMs;
        ExclusiveEndpointProbeResult result{};
        result.probe.compatible = true;
        result.probe.requestedFrames = 240;
        QueueExclusiveEndpointProbeResult(&state, result);
        SendMessageW(hwnd, WM_EXCLUSIVE_ENDPOINT_PROBE_COMPLETE, 0, 0);
        SendMessageW(hwnd, WM_EXCLUSIVE_SCAN_COMPLETE, 0, 0);
        Require(!state.exclusiveScanRunning && state.exclusiveScanCompleted == 1 &&
                state.exclusiveEndpointResults[0].state == ExclusiveEndpointState::Supported,
                "late results retained and completion clears running flag");
        Require(output.resets == 0 && buffer.resets == 0 && status.textWrites == 0 &&
                state.selectedBufferMs == oldBuffer,
                "late Exclusive results never repaint or modify Shared choices");
        selectMode(2);
        Require(!IsWindowEnabled(state.bufferCombo) && !IsWindowEnabled(state.surround51Check),
                "ASIO keeps its driver-owned buffer and disabled surround controls");
        output = {}; buffer = {}; status = {};
        SendMessageW(hwnd, WM_EXCLUSIVE_ENDPOINT_PROBE_COMPLETE, 0, 0);
        SendMessageW(hwnd, WM_AUDIOCLIENT3_PROBE_COMPLETE, 0, 0);
        Require(output.resets == 0 && buffer.resets == 0 && status.textWrites == 0 &&
                !IsWindowEnabled(state.bufferCombo), "late WASAPI messages leave ASIO fields untouched");
        selectMode(1);
        Require(!state.exclusiveProbeThread.joinable() && !state.exclusiveScanRunning &&
                IsWindowEnabled(state.exclusiveTestButton), "completed scan reused on revisit");
        // A temporary failure is completed, but neither approved nor unsupported.
        state.exclusiveScanCompleted = 0;
        state.exclusiveEndpointResults[0].state = ExclusiveEndpointState::Testing;
        result.probe.compatible = false;
        result.probe.inconclusive = true;
        QueueExclusiveEndpointProbeResult(&state, result);
        ConsumeExclusiveEndpointProbeResults(&state);
        Require(state.exclusiveEndpointResults[0].state == ExclusiveEndpointState::RetryRequired,
                "temporary failure is not cached as unsupported");
        PersistCompletedExclusiveEndpointResults(&state);
        Require(FindExclusiveEndpointCache(state.audioEndpoints[0].id)->inconclusive,
                "temporary attempt persists for manual retry only");
        selectMode(0);
        selectMode(1);
        Require(!state.exclusiveProbeThread.joinable() && !state.exclusiveScanRunning &&
                !SettingsCanStart(&state), "mode switch neither retries nor approves a deferred device");
        // Start the real asynchronous path with an empty endpoint list: it
        // posts completion without touching a driver, still exercising the
        // initial idle-to-scanning transition and worker ownership.
        selectMode(0);
        state.audioEndpoints.clear();
        state.exclusiveEndpointResults.clear();
        state.exclusiveScanCompleted = 0;
        button = {}; status = {}; owner = {};
        selectMode(1);
        Require(button.enables == 0 && status.textWrites == 1 && owner.directDraws == 0,
                "first scan shows only its final busy state, not an enabled button or intermediate status");
        Require(state.exclusiveProbeThread.joinable(), "empty scan exercises real worker start");
        state.exclusiveProbeThread.join();
        MSG completion{};
        Require(PeekMessageW(&completion,hwnd,WM_EXCLUSIVE_SCAN_COMPLETE,
                WM_EXCLUSIVE_SCAN_COMPLETE,PM_REMOVE) != FALSE, "empty scan posted completion");
        DispatchMessageW(&completion);
        Require(!state.exclusiveScanRunning && IsWindowEnabled(state.exclusiveTestButton),
                "worker completion restores retry button");
        DestroyWindow(hwnd);
        for (HFONT font : state.uiFonts) DeleteObject(font);
    }
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    g_settings = savedSettings;
    g_suppressSettingsSave = savedSuppress;
    std::printf("PASS: %u real-controller audio mode transitions; scan gates, late results and redraw coalescing.\n", transitions);
}

static std::vector<BYTE> OutputPixels(DirectD3D11Renderer& renderer) {
    D3D11_TEXTURE2D_DESC desc{};
    renderer.backBuffer->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_STAGING; desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ; desc.MiscFlags = 0;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> copy;
    Check(renderer.device->CreateTexture2D(&desc, nullptr, &copy), "readback texture");
    renderer.context->CopyResource(copy.Get(), renderer.backBuffer);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    Check(renderer.context->Map(copy.Get(), 0, D3D11_MAP_READ, 0, &mapped), "readback outside timing");
    std::vector<BYTE> pixels(static_cast<size_t>(desc.Width)*desc.Height*4);
    for (UINT y = 0; y < desc.Height; ++y) {
        const auto* row = static_cast<const BYTE*>(mapped.pData) + y * mapped.RowPitch;
        std::memcpy(pixels.data()+static_cast<size_t>(y)*desc.Width*4,row,static_cast<size_t>(desc.Width)*4);
    }
    renderer.context->Unmap(copy.Get(), 0);
    return pixels;
}
static std::atomic<int> modeQueryCalls{0};
static void TestSettingsModeFlow() {
    const auto saved = g_settings;
    g_testVideoCapabilityProbe = [](const std::wstring&, int width, int, HRESULT* status) {
        ++modeQueryCalls;
        std::this_thread::sleep_for(std::chrono::milliseconds(80)); // slow-driver simulation
        *status = S_OK;
        return std::vector<PixelFormatSupport>{{VideoPixelFormat::Nv12, width == 1280 ? 144 : 60}};
    };
    WNDCLASSW wc{};
    wc.hInstance = GetModuleHandleW(nullptr); wc.lpfnWndProc = SettingsControllerTestProc;
    wc.lpszClassName = L"LLCV_SETTINGS_ASYNC_TEST";
    Require(RegisterClassW(&wc) != 0, "async test class");
    SettingsDialogState state;
    state.activeTab = SettingsTab::VideoWindow;
    HWND window = CreateWindowW(wc.lpszClassName, L"Async fixture", WS_POPUP,
        0,0,1000,650,nullptr,nullptr,wc.hInstance,&state);
    auto combo = [&](int id) {
        return CreateWindowW(L"COMBOBOX",L"",WS_CHILD | CBS_DROPDOWNLIST,
            0,0,200,120,window,reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),wc.hInstance,nullptr);
    };
    state.videoCombo = combo(IDC_SETTINGS_VIDEO);
    state.displayResolutionCombo = combo(IDC_SETTINGS_DISPLAY_RESOLUTION);
    state.pixelFormatCombo = combo(IDC_SETTINGS_PIXEL_FORMAT);
    state.frameRateCombo = combo(IDC_SETTINGS_FRAME_RATE);
    state.unlistedFpsCheck = CreateWindowW(L"BUTTON",L"",WS_CHILD | BS_AUTOCHECKBOX,
        0,0,240,28,window,nullptr,wc.hInstance,nullptr);
    state.videoCapabilityStatus = CreateWindowW(L"STATIC",L"",WS_CHILD,
        0,0,376,76,window,nullptr,wc.hInstance,nullptr);
    state.unlistedFpsStatus = CreateWindowW(L"STATIC",L"",WS_CHILD,
        0,0,376,16,window,nullptr,wc.hInstance,nullptr);
    state.vsrCheck = CreateWindowW(L"BUTTON",L"",WS_CHILD | BS_AUTOCHECKBOX,
        0,0,200,30,window,nullptr,wc.hInstance,nullptr);
    state.startButton = CreateWindowW(L"BUTTON",L"Start",WS_CHILD,
        0,0,100,30,window,nullptr,wc.hInstance,nullptr);
    // Manual Refresh also discovers device inventory. Keep this regression
    // entirely synthetic, including its internal capture-audio follow-up.
    unsigned inventoryQueries = 0;
    state.inventoryCache = std::make_unique<llcv::settings_ui::SettingsModeCache<SettingsDeviceInventory>>(
        [&](const auto&) { ++inventoryQueries; return SettingsDeviceInventory{}; },
        [window] { PostMessageW(window, WM_SETTINGS_INVENTORY_READY, 0, 0); });
    state.captureAudioProbeCache = std::make_unique<llcv::settings_ui::SettingsModeCache<InternalCaptureAudioProbe>>(
        [](const auto&) { return InternalCaptureAudioProbe{}; },
        [window] { PostMessageW(window, WM_CAPTURE_AUDIO_PROBE_COMPLETE, 0, 0); });
    for (const auto& preset : kVideoPresets) {
        SendMessageW(state.videoCombo,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(preset.label));
        SendMessageW(state.displayResolutionCombo,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(preset.label));
    }
    SendMessageW(state.vsrCheck,BM_SETCHECK,BST_CHECKED,0);
    SendMessageW(state.displayResolutionCombo,CB_SETCURSEL,2,0);
    SendMessageW(state.videoCombo,CB_SETCURSEL,0,0);
    auto complete = [&] {
        const auto deadline = GetTickCount64() + 2000;
        while ((state.inventoryPending || state.videoModesPending) && GetTickCount64() < deadline) {
            MsgWaitForMultipleObjects(0,nullptr,FALSE,20,QS_ALLINPUT);
            MSG msg{}; while (PeekMessageW(&msg,window,0,0,PM_REMOVE)) DispatchMessageW(&msg);
        }
        Require(!state.videoModesPending && IsWindowEnabled(state.startButton), "current modes arrive asynchronously");
    };
    PopulatePixelFormatCombo(&state);
    Require(state.videoModesPending && !IsWindowEnabled(state.startButton), "cannot start with stale modes");
    complete();
    Require(modeQueryCalls == 1 && state.pixelFormats.front().selectedFps == 144, "FPS follows capture size");
    const auto rateRows=[&](int rate) {
        int count=0;
        for(LRESULT i=0;i<SendMessageW(state.frameRateCombo,CB_GETCOUNT,0,0);++i)
            if(SendMessageW(state.frameRateCombo,CB_GETITEMDATA,i,0)==rate)++count;
        return count;
    };
    Require(rateRows(30)==0, "unlisted 30fps is hidden by default");
    Require(GetWindowTextLengthW(state.unlistedFpsStatus)==0,
        "direct request hint is empty by default");
    SendMessageW(state.unlistedFpsCheck,BM_SETCHECK,BST_CHECKED,0);
    SendMessageW(window,WM_COMMAND,MAKEWPARAM(IDC_SETTINGS_UNLISTED_FPS,BN_CLICKED),0);
    Require(rateRows(30)==1 && state.pixelFormats.size()==1,
        "unlisted 30fps appears once without modifying detected capabilities");
    Require(GetWindowTextLengthW(state.unlistedFpsStatus)>0,
        "unlisted 30fps opt-in shows distinct direct request hint");
    const auto countBefore=modeQueryCalls.load();
    g_settings.capture40Fps=false;
    g_settings.videoFrameRate=30;
    SendMessageW(state.frameRateCombo,CB_SETCURSEL,static_cast<WPARAM>(-1),0);
    PopulateFrameRateCombo(&state);
    Require(SendMessageW(state.frameRateCombo,CB_GETITEMDATA,
        SendMessageW(state.frameRateCombo,CB_GETCURSEL,0,0),0)==30 &&
        RequestsExactCaptureRate() && RequestedVideoFrameRate()==30,
        "saved 30fps is retained and requests strict negotiation");
    g_settings.capture40Fps=true;
    Require(RequestedVideoFrameRate()==40 && RequestsExactCaptureRate(),
        "40fps override remains authoritative over selected 30fps");
    g_settings.capture40Fps=false;
    SendMessageW(state.unlistedFpsCheck,BM_SETCHECK,BST_UNCHECKED,0);
    SendMessageW(window,WM_COMMAND,MAKEWPARAM(IDC_SETTINGS_UNLISTED_FPS,BN_CLICKED),0);
    Require(rateRows(30)==1 && SelectedCaptureFrameRate(&state)==30 && !SettingsCanStart(&state),
        "disabling unlisted rates preserves 30fps visibly but blocks unsupported Start");
    Require(GetWindowTextLengthW(state.unlistedFpsStatus)>0,
        "disabling unlisted rates explains the unavailable selection");
    g_settings.videoFrameRate=0;
    Require(!RequestsExactCaptureRate(),"automatic frame rate does not enable direct-request policy");
    state.pixelFormats.push_back({VideoPixelFormat::Nv12,30});
    PopulateFrameRateCombo(&state);
    Require(rateRows(30)==1 && modeQueryCalls==countBefore,
        "driver-listed 30fps remains available with opt-in off and no extra queries");
    SendMessageW(state.frameRateCombo,CB_SETCURSEL,2,0);
    SendMessageW(state.unlistedFpsCheck,BM_SETCHECK,BST_CHECKED,0);
    PopulateFrameRateCombo(&state);
    Require(rateRows(30)==1,"driver-listed 30fps is not duplicated when opted in");
    UpdateVideoCapabilityStatus(&state);
    Require(GetWindowTextLengthW(state.unlistedFpsStatus)==0,
        "driver-listed 30fps never appears as a synthetic request");
    SendMessageW(state.unlistedFpsCheck,BM_SETCHECK,BST_UNCHECKED,0);
    state.pixelFormats.pop_back();
    SendMessageW(state.frameRateCombo,CB_SETCURSEL,0,0); // Deliberately return to Auto for sizing tests.
    for (int i = 0; i < 50; ++i) {
        SendMessageW(state.displayResolutionCombo,CB_SETCURSEL,1 + i % 3,0);
        SendMessageW(window,WM_COMMAND,MAKEWPARAM(IDC_SETTINGS_DISPLAY_RESOLUTION,CBN_SELCHANGE),0);
    }
    Require(modeQueryCalls == 1 && !state.videoModesPending, "display changes never rescan or clear FPS");
    SendMessageW(state.vsrCheck,BM_SETCHECK,BST_UNCHECKED,0);
    SendMessageW(window,WM_COMMAND,MAKEWPARAM(IDC_SETTINGS_VSR,BN_CLICKED),0);
    complete();
    SendMessageW(state.vsrCheck,BM_SETCHECK,BST_CHECKED,0);
    SendMessageW(window,WM_COMMAND,MAKEWPARAM(IDC_SETTINGS_VSR,BN_CLICKED),0);
    Require(modeQueryCalls == 1 && !state.videoModesPending &&
        state.pixelFormats.front().selectedFps == 144,
        "VSR on/off preserves capture selection and never rescans capabilities");
    SendMessageW(state.videoCombo,CB_SETCURSEL,2,0);
    SendMessageW(window,WM_COMMAND,MAKEWPARAM(IDC_SETTINGS_VIDEO,CBN_SELCHANGE),0);
    complete();
    Require(modeQueryCalls == 2 && state.pixelFormats.front().selectedFps == 60,
        "only the capture field changes the capability query");
    SendMessageW(state.videoCombo,CB_SETCURSEL,0,0);
    SendMessageW(window,WM_COMMAND,MAKEWPARAM(IDC_SETTINGS_VIDEO,CBN_SELCHANGE),0);
    Require(modeQueryCalls == 2 && !state.videoModesPending &&
        state.pixelFormats.front().selectedFps == 144, "revisiting capture mode uses cached capabilities");
    SendMessageW(window,WM_COMMAND,MAKEWPARAM(IDC_SETTINGS_VIDEO_REFRESH,BN_CLICKED),0);
    complete();
    Require(modeQueryCalls == 3 && inventoryQueries == 1, "only explicit refresh repeats the same query and enumerates inventory");
    state.vsrGpuUnavailable = true;
    // GPU capability gates only the effect, never capture or display sizing.
    SendMessageW(state.vsrCheck, BM_SETCHECK, BST_CHECKED, 0);
    SendMessageW(window, WM_COMMAND, MAKEWPARAM(IDC_SETTINGS_VSR, BN_CLICKED), 0);
    Require(modeQueryCalls == 3 && !state.videoModesPending &&
        SendMessageW(state.vsrCheck, BM_GETCHECK, 0, 0) == BST_UNCHECKED &&
        !IsWindowEnabled(state.vsrCheck), "blocked VSR notification cannot rescan or enable");
    SendMessageW(state.displayResolutionCombo, CB_SETCURSEL, 2, 0);
    SendMessageW(window, WM_COMMAND, MAKEWPARAM(IDC_SETTINGS_DISPLAY_RESOLUTION, CBN_SELCHANGE), 0);
    auto applied = g_settings;
    llcv::settings_ui::ApplyResolutionSelections(state, kVideoPresets, applied);
    Require(IsWindowEnabled(state.displayResolutionCombo) && modeQueryCalls == 3 &&
        applied.capturePreset == kVideoPresets[0].preset && applied.videoPreset == kVideoPresets[2].preset,
        "non-NVIDIA display selection remains editable and saves independently without rescanning");
    const auto matchRow = SendMessageW(state.displayResolutionCombo, CB_ADDSTRING, 0,
        reinterpret_cast<LPARAM>(L"Same as capture"));
    SendMessageW(state.displayResolutionCombo, CB_SETITEMDATA, matchRow, llcv::settings_ui::kDisplayMatchCaptureItem);
    const auto oldFrameRate = SelectedCaptureFrameRate(&state);
    for (int repeat = 0; repeat < 20; ++repeat) {
        SendMessageW(state.displayResolutionCombo, CB_SETCURSEL, repeat % 2 ? 2 : matchRow, 0);
        SendMessageW(window, WM_COMMAND, MAKEWPARAM(IDC_SETTINGS_DISPLAY_RESOLUTION, CBN_SELCHANGE), 0);
        llcv::settings_ui::ApplyResolutionSelections(state, kVideoPresets, applied);
        Require(applied.displayMatchesCapture == (repeat % 2 == 0) &&
                llcv::settings::EffectiveDisplayPreset(applied) == kVideoPresets[repeat % 2 ? 2 : 0].preset &&
                applied.videoPreset == kVideoPresets[2].preset && modeQueryCalls == 3 &&
                SelectedCaptureFrameRate(&state) == oldFrameRate,
            "same-as-capture switching preserves fixed preference, FPS and cached device capabilities");
    }
    SendMessageW(state.vsrCheck, BM_SETCHECK, BST_CHECKED, 0);
    PopulatePixelFormatCombo(&state);
    complete();
    Require(modeQueryCalls == 3 && state.pixelFormats.front().selectedFps == 144,
        "unavailable VSR preserves the capture field even with a stale check");
    state.videoModeCache.reset();
    state.inventoryCache.reset();
    state.captureAudioProbeCache.reset();
    DestroyWindow(window);
    UnregisterClassW(wc.lpszClassName,wc.hInstance);
    g_testVideoCapabilityProbe = nullptr;
    g_settings = saved;
    std::puts("PASS: production settings flow, slow driver, 50 display changes without rescans, cached capture/FPS and refresh.");
}

static uint64_t OutputHash(DirectD3D11Renderer& renderer) {
    uint64_t hash = 14695981039346656037ull;
    for (const auto value : OutputPixels(renderer)) hash = (hash ^ value) * 1099511628211ull;
    return hash;
}
static void PrintPixelDifference(const std::vector<BYTE>& a, const std::vector<BYTE>& b,
                                 const char* label) {
    Require(a.size() == b.size() && !a.empty(), "pixel comparison dimensions");
    uint64_t changed = 0, total = 0;
    int maximum = 0;
    for (size_t i = 0; i < a.size(); i += 4) {
        bool different = false;
        for (size_t channel = 0; channel < 3; ++channel) {
            const int delta = std::abs(static_cast<int>(a[i+channel])-b[i+channel]);
            different |= delta != 0; total += delta; maximum = (std::max)(maximum,delta);
        }
        changed += different;
    }
    std::printf("PIXELS %s changed=%llu/%llu (%.4f%%) RGB_MAE=%.6f max=%d/255\n",
        label,changed,static_cast<uint64_t>(a.size()/4),100.0*changed/(a.size()/4),
        static_cast<double>(total)/(a.size()/4*3),maximum);
}

static void TestNativeControls(HWND window, int width, int height, const std::vector<BYTE>& pixels) {
    g_vsrMode = llcv::vsr::Mode::Disabled;
    DirectD3D11Renderer renderer;
    Check(renderer.initialize(window,width,height,60,VideoPixelFormat::Nv12), "native controls init");
    Require(renderer.vsrEligible && renderer.vsrState == llcv::vsr::State::Untouched,
        "native controls eligible but default processing remains untouched");
    const auto* device = renderer.device;
    const auto* processor = renderer.processor;
    const auto* swapchain = renderer.swapChain;
    auto render = [&] {
        // Warm repeated static input outside any timing measurement.
        for (int i = 0; i < 12; ++i) {
            renderer.upload(pixels.data(),static_cast<UINT32>(width));
            D3D11_VIDEO_PROCESSOR_STREAM stream{};
            stream.Enable = TRUE; stream.pInputSurface = renderer.inputViews[renderer.activeUploadSurface];
            Check(renderer.videoContext->VideoProcessorBlt(renderer.processor,renderer.outputView,0,1,&stream),
                "native live control blit");
            renderer.context->Flush();
        }
        return OutputPixels(renderer);
    };
    const auto baseline = render();
    for (int pair = 0; pair < 3; ++pair) {
        g_vsrMode = llcv::vsr::Mode::On;
        Check(renderer.applyVsrMode(), "native live ON");
        Require(renderer.vsrState == llcv::vsr::State::Requested, "native live ON accepted");
        PrintPixelDifference(baseline,render(),"native live default vs ON");
        g_vsrMode = llcv::vsr::Mode::Off;
        Check(renderer.applyVsrMode(), "native live OFF");
        Require(renderer.vsrState == llcv::vsr::State::Off, "native live OFF accepted");
        Require(render() == baseline, "native OFF restores untouched default pixels exactly");
        Require(renderer.device == device && renderer.processor == processor && renderer.swapChain == swapchain,
            "native live toggles preserve all rendering resources");
    }
    std::puts("PASS: native default/OFF exact equality and three same-resource ON/OFF controls.");
    g_vsrMode = llcv::vsr::Mode::Disabled;
}

static void TestLiveToggle(HWND window, int width, int height, const std::vector<BYTE>& pixels,
                           VideoPixelFormat format = VideoPixelFormat::Nv12) {
    g_vsrMode.store(llcv::vsr::Mode::Disabled);
    DirectD3D11Renderer renderer;
    Check(renderer.initialize(window,width,height,60,format), "toggle renderer init");
    const auto* originalDevice = renderer.device;
    const auto* originalProcessor = renderer.processor;
    const auto* originalSwapchain = renderer.swapChain;
    const bool originalVsrEligible = renderer.vsrEligible;
    const auto originalGeneration = g_outputConfigurationGeneration.load();
    const auto originalRendererGeneration = renderer.outputConfigurationGeneration;
    auto renderHash = [&] {
        renderer.upload(pixels.data(), static_cast<UINT32>(width*(format == VideoPixelFormat::Yuy2 ? 2 : 1)));
        D3D11_VIDEO_PROCESSOR_STREAM stream{};
        stream.Enable = TRUE; stream.pInputSurface = renderer.inputViews[renderer.activeUploadSurface];
        Check(renderer.videoContext->VideoProcessorBlt(renderer.processor,renderer.outputView,0,1,&stream),
            "same-resource toggle blit");
        return OutputHash(renderer);
    };
    const uint64_t baseline = renderHash();
    if (!llcv::vsr::NvidiaAdapter(g_vsrGpuVendor.load())) {
        const bool savedPreference = g_settings.vsrEnabled;
        g_settings.audioOnly = false;
        Require(HandleVsrTestKey(VK_F6, 0, 0) &&
            g_vsrMode.load() == llcv::vsr::Mode::Disabled &&
            g_settings.vsrEnabled == savedPreference &&
            renderHash() == baseline, "actual non-NVIDIA renderer rejects F6 without changing video");
        return;
    }
    Require(!HandleVsrTestKey(VK_F5,0,0), "other shortcuts untouched");
    g_settings.audioOnly = true;
    Require(HandleVsrTestKey(VK_F6,0,0) && g_vsrMode.load() == llcv::vsr::Mode::Disabled,
        "audio-only does not change VSR request");
    const auto originalBindings=g_settings.shortcuts;
    g_settings.shortcuts[static_cast<size_t>(keys::Action::Vsr)]=keys::Ctrl|'V';
    Require(!HandleVsrTestKey(VK_F6,0,0) && !HandleVsrTestKey('V',0,0) &&
        HandleVsrTestKey('V',0,keys::Ctrl) && g_vsrMode.load()==llcv::vsr::Mode::Disabled,
        "custom VSR combination replaces F6 and preserves the audio-only gate");
    g_settings.shortcuts=originalBindings;
    g_settings.audioOnly = false;
    for (int i = 0; i < 6; ++i) {
        const auto stateBeforeKey = renderer.vsrState;
        Require(HandleVsrTestKey(VK_F6,0,0), "F6 handled");
        Require(g_settings.vsrEnabled, "F6 ON updates saved preference");
        Require(g_vsrMode.load() == llcv::vsr::Mode::On && renderer.vsrState == stateBeforeKey,
            "UI updates request only, not D3D state");
        Require(HandleVsrTestKey(VK_F6,LPARAM{1} << 30,0) && g_vsrMode.load() == llcv::vsr::Mode::On,
            "held F6 ignores auto-repeat");
        Check(renderer.applyVsrMode(), "ON on render thread");
        Require(renderer.cachedOverlayGeneration == 0, "applied F6 invalidates Tab status cache");
        Require(renderer.vsrState == (renderer.vsrEligible ? llcv::vsr::State::Requested
                                                          : llcv::vsr::State::Bypassed), "ON state");
        const uint64_t enabled = renderHash();
        std::printf("live pair=%d eligible=%d ON differs=%d\n",i,renderer.vsrEligible,enabled != baseline);
        // HUD must use the acknowledged render state, not claim activation.
        Require(g_transientHudContent.load() == (renderer.vsrEligible ? TransientHudContent::VsrOn
            : TransientHudContent::VsrUnavailable), "acknowledged HUD");
        const auto overlayGeneration = g_overlayGeneration.load();
        Check(renderer.applyVsrMode(), "unchanged mode no-op");
        Require(g_overlayGeneration.load() == overlayGeneration, "no per-frame status churn");
        HandleVsrTestKey(VK_F6,0,0);
        Check(renderer.applyVsrMode(), "OFF on render thread");
        Require(!g_settings.vsrEnabled, "F6 OFF updates saved preference");
        Require(renderHash() == baseline, "live OFF restores original pixels exactly");
        Require(renderer.device == originalDevice && renderer.processor == originalProcessor &&
            renderer.swapChain == originalSwapchain &&
            renderer.outputConfigurationGeneration == originalRendererGeneration &&
            g_outputConfigurationGeneration.load() == originalGeneration, "toggle never rebuilds output");
    }
    HandleVsrTestKey(VK_F6,0,0); HandleVsrTestKey(VK_F6,0,0);
    Check(renderer.applyVsrMode(), "rapid toggles coalesce to last request");
    Require(g_vsrMode.load() == llcv::vsr::Mode::Off && renderHash() == baseline, "rapid OFF result");
    // Exercise actual Present/HUD path (a hidden window may be occluded).
    HandleVsrTestKey(VK_F6,0,0);
    Check(renderer.presentUploaded(), "production Present handles pending toggle");
    Require(renderer.vsrAppliedMode == llcv::vsr::Mode::On, "Present consumes request");
    HandleVsrTestKey(VK_F6,0,0);
    Check(renderer.presentUploaded(), "OFF while occluded is consumed");
    Require(renderer.vsrAppliedMode == llcv::vsr::Mode::Off, "occlusion does not lose request");
    HandleVsrTestKey(VK_F6,0,0); // preserve ON intent through real output changes
    Require(SetWindowPos(window,nullptr,0,0,width,height,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE) != 0,
        "hidden 1:1 output");
    Check(renderer.initialize(window,width,height,60,format), "recreate 1:1");
    Require(renderer.vsrEligible == originalVsrEligible &&
        renderer.vsrAppliedMode == llcv::vsr::Mode::On &&
        renderer.vsrState == (renderer.vsrEligible ? llcv::vsr::State::Requested : llcv::vsr::State::Bypassed) &&
        g_transientHudContent.load() == (renderer.vsrEligible ? TransientHudContent::VsrOn
            : TransientHudContent::VsrUnavailable), "native-size ON intent and acknowledged HUD");
    Require(SetWindowPos(window,nullptr,0,0,width/2,height/2,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE) != 0,
        "hidden downscaled output");
    Check(renderer.initialize(window,width,height,60,format), "recreate downscale");
    Require(!renderer.vsrEligible && renderer.vsrState == llcv::vsr::State::Bypassed &&
        g_transientHudContent.load() == TransientHudContent::VsrUnavailable, "downscale bypass with visible explanation");
    Require(SetWindowPos(window,nullptr,0,0,width*2,height*2,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE) != 0,
        "restore hidden upscale output");
    Check(renderer.initialize(window,width,height,60,format), "recreate upscale");
    Require(renderer.vsrAppliedMode == llcv::vsr::Mode::On &&
        renderer.vsrState == (renderer.vsrEligible ? llcv::vsr::State::Requested : llcv::vsr::State::Bypassed),
        "ON intent reapplied after actual resize");
    const auto savedLanguage = g_settings.uiLanguage;
    for (const auto language : {UiLanguage::Korean, UiLanguage::English}) {
        g_settings.uiLanguage = language;
        const auto savedState = renderer.vsrState;
        renderer.vsrState = llcv::vsr::State::Requested;
        const auto onLine = renderer.vsrOsdLine();
        Require(onLine.find(L"ON") != std::wstring::npos &&
            onLine.find(L"requested") == std::wstring::npos &&
            onLine.find(L"요청") == std::wstring::npos, "Tab ON uses concise label");
        renderer.vsrState = savedState;
        for (const auto content : {TransientHudContent::VsrPending, TransientHudContent::VsrOn,
                TransientHudContent::VsrOff, TransientHudContent::VsrUnavailable,
                TransientHudContent::VsrRejected, TransientHudContent::VsrFailed}) {
            ShowTransientHud(content);
            Check(renderer.refreshOverlayLayouts(), "bilingual VSR overlay");
            DWRITE_TEXT_METRICS metrics{};
            Check(renderer.volumeTextLayout->GetMetrics(&metrics), "VSR HUD text metrics");
            std::printf("HUD language=%d content=%d width=%.2f height=%.2f lines=%u\n",
                static_cast<int>(language), static_cast<int>(content), metrics.width, metrics.height, metrics.lineCount);
            Require(metrics.height <= 62.0f && metrics.width <= 228.0f && metrics.lineCount <= 2,
                "VSR messages fit existing HUD without clipping");
        }
    }
    g_settings.uiLanguage = savedLanguage;
    g_vsrMode = llcv::vsr::Mode::Disabled;
}

static void TestCenteredPixelPerfect(HWND window) {
    const int width = 1280, height = 720; // An actual selectable display size.
    std::vector<BYTE> pixels(static_cast<size_t>(width) * height * 3 / 2, 128);
    const auto savedDisplay = g_settings.videoPreset;
    const auto savedCapture = g_settings.capturePreset;
    const bool savedMatchCapture = g_settings.displayMatchesCapture;
    const auto savedPosition = g_settings.fullscreenVideoPosition;
    g_settings.videoPreset = g_settings.capturePreset = VideoPreset::R1280x720;
    g_settings.fullscreenVideoPosition = llcv::settings::FullscreenVideoPosition::Center;
    const bool savedPixelPerfect = g_settings.pixelPerfect;
    const auto savedScaling = g_settings.scalingMode;
    const bool savedFullscreen = g_fullscreen.load();
    const auto savedMode = g_vsrMode.load();
    g_settings.pixelPerfect = true;
    g_settings.scalingMode = ScalingMode::Sharp; // Native-size output must not add edge enhancement.
    g_fullscreen = true;
    g_vsrMode = llcv::vsr::Mode::Disabled;
    Require(SetWindowPos(window,nullptr,0,0,width*2,height*2,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE) != 0,
        "centered pixel-perfect hidden output");
    DirectD3D11Renderer renderer;
    Check(renderer.initialize(window,width,height,60,VideoPixelFormat::Nv12), "centered pixel-perfect init");
    Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
    Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
    DXGI_ADAPTER_DESC desc{};
    Check(renderer.device->QueryInterface(IID_PPV_ARGS(&dxgiDevice)), "centered adapter device");
    Check(dxgiDevice->GetAdapter(&adapter), "centered adapter");
    Check(adapter->GetDesc(&desc), "centered adapter description");
    const bool expectedEligible = llcv::vsr::Eligible(desc.VendorId,DXGI_FORMAT_NV12,false,
        static_cast<UINT>(width),static_cast<UINT>(height),static_cast<UINT>(width),static_cast<UINT>(height));
    Require(renderer.fullscreenDisplaySize && renderer.pixelPerfectBorders &&
        renderer.outputWidth == static_cast<UINT>(width*2) &&
        renderer.outputHeight == static_cast<UINT>(height*2) &&
        renderer.vsrEligible == expectedEligible && !renderer.sharpScalingActive &&
        renderer.vsrState == llcv::vsr::State::Untouched,
        "centered native policy uses video rectangle, not larger backbuffer");
    const RECT sourceExpected{0,0,width,height};
    const RECT nativeExpected{width/2,height/2,width/2+width,height/2+height};
    auto requireRects = [&](const RECT& destination, const RECT& target) {
        BOOL sourceEnabled = FALSE, destEnabled = FALSE, targetEnabled = FALSE;
        RECT source{}, dest{}, output{};
        renderer.videoContext->VideoProcessorGetStreamSourceRect(renderer.processor,0,&sourceEnabled,&source);
        renderer.videoContext->VideoProcessorGetStreamDestRect(renderer.processor,0,&destEnabled,&dest);
        renderer.videoContext->VideoProcessorGetOutputTargetRect(renderer.processor,&targetEnabled,&output);
        Require(sourceEnabled && EqualRect(&source,&sourceExpected) &&
            destEnabled && EqualRect(&dest,&destination) && targetEnabled && EqualRect(&output,&target),
            "source/destination/target rectangles preserve pixel-perfect placement");
    };
    const RECT largeTarget{0,0,width*2,height*2};
    requireRects(nativeExpected,largeTarget);
    const auto* device = renderer.device;
    const auto* processor = renderer.processor;
    const auto* swapchain = renderer.swapChain;
    auto renderNativeRegion = [&] {
        renderer.upload(pixels.data(),static_cast<UINT32>(width));
        D3D11_VIDEO_PROCESSOR_STREAM stream{};
        stream.Enable = TRUE; stream.pInputSurface = renderer.inputViews[renderer.activeUploadSurface];
        Check(renderer.videoContext->VideoProcessorBlt(renderer.processor,renderer.outputView,0,1,&stream),
            "centered native blit");
        const auto output = OutputPixels(renderer);
        std::vector<BYTE> region(static_cast<size_t>(width)*height*4);
        // Direct test blits need not clear the letterbox region. Compare only
        // the queried, fully written video rectangle, never stale border RGB.
        for (int row = 0; row < height; ++row) {
            const size_t sourceOffset = (static_cast<size_t>(nativeExpected.top+row)*renderer.outputWidth+
                nativeExpected.left)*4;
            std::memcpy(region.data()+static_cast<size_t>(row)*width*4,
                output.data()+sourceOffset,static_cast<size_t>(width)*4);
        }
        return region;
    };
    const auto baseline = renderNativeRegion();
    g_settings.videoPreset = VideoPreset::R3840x2160;
    g_settings.displayMatchesCapture = true;
    Check(renderer.configureVideoOutput(width,height), "match-capture fullscreen output");
    requireRects(nativeExpected,largeTarget);
    Require(renderNativeRegion() == baseline && g_settings.videoPreset == VideoPreset::R3840x2160 &&
            renderer.device == device && renderer.processor == processor && renderer.swapChain == swapchain,
        "same-as-capture preserves native fullscreen pixels and resources without erasing the fixed display size");
    for (int pair = 0; pair < 3; ++pair) {
        g_vsrMode = llcv::vsr::Mode::On;
        Check(renderer.applyVsrMode(), "centered native ON");
        Require(renderer.vsrState == (expectedEligible ? llcv::vsr::State::Requested
            : llcv::vsr::State::Bypassed), "centered native ON acknowledged");
        requireRects(nativeExpected,largeTarget);
        Require(renderer.vsrInputWidth == static_cast<UINT>(width) &&
            renderer.vsrInputHeight == static_cast<UINT>(height) &&
            renderer.vsrDisplayWidth == static_cast<UINT>(width) &&
            renderer.vsrDisplayHeight == static_cast<UINT>(height),
            "Tab VSR sizes exclude fullscreen pixel-perfect borders");
        PrintPixelDifference(baseline,renderNativeRegion(),"centered native default vs ON");
        g_vsrMode = llcv::vsr::Mode::Off;
        Check(renderer.applyVsrMode(), "centered native OFF");
        Require(renderer.vsrState == (expectedEligible ? llcv::vsr::State::Off
            : llcv::vsr::State::Bypassed), "centered native OFF acknowledged");
        Require(renderNativeRegion() == baseline, "centered native OFF restores written video region");
        requireRects(nativeExpected,largeTarget);
        Require(renderer.device == device && renderer.processor == processor && renderer.swapChain == swapchain,
            "centered native toggles preserve rendering resources");
    }
    g_vsrMode = llcv::vsr::Mode::On;
    Require(SetWindowPos(window,nullptr,0,0,width/2,height/2,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE) != 0,
        "oversized pixel-perfect input hidden output");
    Check(renderer.initialize(window,width,height,60,VideoPixelFormat::Nv12), "pixel-perfect downscale init");
    const RECT smallTarget{0,0,width/2,height/2};
    requireRects(smallTarget,smallTarget);
    Require(renderer.fullscreenDisplaySize && !renderer.pixelPerfectBorders && !renderer.vsrEligible &&
        !renderer.sharpScalingActive && renderer.vsrState == llcv::vsr::State::Bypassed &&
        renderer.vsrAppliedMode == llcv::vsr::Mode::On,
        "pixel-perfect downscale bypasses VSR while retaining ON intent");
    renderer.reset();
    g_settings.pixelPerfect = savedPixelPerfect;
    g_settings.scalingMode = savedScaling;
    g_fullscreen = savedFullscreen;
    g_vsrMode = savedMode;
    std::puts("PASS: centered fullscreen 1:1 rectangles, ON/OFF native pixels, resources, and oversized-input bypass.");
    g_settings.videoPreset = savedDisplay;
    g_settings.capturePreset = savedCapture;
    g_settings.displayMatchesCapture = savedMatchCapture;
    g_settings.fullscreenVideoPosition = savedPosition;
}

static void TestSplitResolutionRenderer(HWND window) {
    const auto saved = g_settings;
    const bool fullscreen = g_fullscreen.load();
    for (const auto display : {VideoPreset::R2560x1440, VideoPreset::R3840x2160}) {
        g_settings.videoPreset = display;
        g_settings.capturePreset = VideoPreset::R1280x720;
        g_settings.vsrEnabled = true;
        g_settings.pixelPerfect = true; // UI now locks DISPLAY size, not source pixels.
        g_settings.audioOnly = false;
        LatchVideoResolutionPlan();
        const auto size = InitialClientPixelsForMonitor(nullptr);
        SetWindowPos(window, nullptr, 0, 0, size.cx, size.cy, SWP_NOZORDER | SWP_NOACTIVATE);
        g_fullscreen = true;
        g_vsrMode = llcv::vsr::Mode::Off;
        DirectD3D11Renderer renderer;
        Check(renderer.initialize(window, 1280, 720, 60, VideoPixelFormat::Nv12), "split renderer");
        Require(renderer.fullscreenDisplaySize && renderer.outputWidth == static_cast<UINT>(size.cx) &&
            renderer.outputHeight == static_cast<UINT>(size.cy) && renderer.vsrEligible,
            "split VSR display fills chosen output even with display lock");
        auto* chain = renderer.swapChain;
        for (int i = 0; i < 12; ++i) {
            HandleVsrTestKey(VK_F6, 0, 0);
            Check(renderer.applyVsrMode(false), "split F6 request");
            Require(renderer.swapChain == chain && CurrentCapturePreset().width == 1280 &&
                renderer.outputWidth == static_cast<UINT>(size.cx) && renderer.outputHeight == static_cast<UINT>(size.cy),
                "split F6 preserves swap chain and dimensions");
        }
    }
    g_settings = saved;
    g_fullscreen = fullscreen;
    g_resolutionPlanLatched = false;
    g_splitResolutionPlan = false;
    g_vsrMode = llcv::vsr::Mode::Disabled;
}

#include "VsrTimingValidation.inl"
#include "VideoClearRegression.inl"

#include "SettingsControllerRegressionTests.inl"
#include "CaptureFrameRateOsdTests.inl"
#include "ViewerLifecycleTests.inl"
#include "ExclusiveViewerLifecycleTests.inl"
#include "ExclusiveRendererRegression.inl"

static void TestPlaybackPowerBroadcastRouting() {
    struct Request {
        bool suspended = false;
        int resumed = 0;
        void Suspend() { suspended = true; }
        void Resume() { suspended = false; ++resumed; }
    } request;
    Require(g_playbackPowerEvents.Initialize(true) == ERROR_SUCCESS, "initialize viewer power notification fixture");
    uint64_t revision = 0;
    Require(WndProc(nullptr, WM_POWERBROADCAST, PBT_APMSUSPEND, 0) == TRUE,
            "viewer accepts the explicit suspend notification");
    Require(WaitForSingleObject(g_playbackPowerEvents.Event(), 0) == WAIT_OBJECT_0 &&
            g_playbackPowerEvents.ApplyPending(request, revision) && request.suspended,
            "viewer forwards suspend to capture instead of altering Windows policy");
    WndProc(nullptr, WM_POWERBROADCAST, PBT_APMRESUMEAUTOMATIC, 0);
    Require(WaitForSingleObject(g_playbackPowerEvents.Event(), 0) == WAIT_TIMEOUT && request.resumed == 0,
            "viewer ignores unattended resume");
    WndProc(nullptr, WM_POWERBROADCAST, PBT_APMRESUMESUSPEND, 0);
    Require(WaitForSingleObject(g_playbackPowerEvents.Event(), 0) == WAIT_OBJECT_0 &&
            g_playbackPowerEvents.ApplyPending(request, revision) && !request.suspended && request.resumed == 1,
            "viewer forwards user resume exactly once");
    WndProc(nullptr, WM_POWERBROADCAST, PBT_APMRESUMESUSPEND, 0);
    Require(WaitForSingleObject(g_playbackPowerEvents.Event(), 0) == WAIT_TIMEOUT,
            "duplicate window broadcasts do not rearm capture");
}

int main(int argc, char** argv) {
    if (argc > 1 && std::string(argv[1]) == "--capture-fps-osd") {
        Check(CoInitializeEx(nullptr, COINIT_MULTITHREADED), "capture FPS OSD COM");
        TestCaptureFrameRateOsd();
        CoUninitialize();
        return 0;
    }
    if (argc > 1 && std::string(argv[1]) == "--exclusive-renderer-regression") {
        Check(CoInitializeEx(nullptr, COINIT_MULTITHREADED), "exclusive renderer COM");
        const int result = RunExclusiveRendererRegression();
        CoUninitialize();
        return result;
    }
    if (argc > 1 && std::string(argv[1]) == "--exclusive-viewer-lifecycle") {
        Check(CoInitializeEx(nullptr, COINIT_MULTITHREADED), "exclusive lifecycle COM");
        INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_WIN95_CLASSES};
        Require(InitCommonControlsEx(&controls) != FALSE, "exclusive lifecycle common controls");
        TestExclusiveViewerLifecycle();
        CoUninitialize();
        return 0;
    }
    if (argc > 1 && std::string(argv[1]) == "--video-clear-regression") {
        Check(CoInitializeEx(nullptr, COINIT_MULTITHREADED), "clear regression COM");
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        const int result = RunVideoClearRegressionTests();
        CoUninitialize();
        return result;
    }
    if (argc > 1 && std::string(argv[1]) == "--viewer-lifecycle") {
        Check(CoInitializeEx(nullptr, COINIT_MULTITHREADED), "lifecycle COM");
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_WIN95_CLASSES};
        Require(InitCommonControlsEx(&controls) != FALSE, "lifecycle common controls");
        TestViewerLifecycle();
        CoUninitialize();
        return 0;
    }
    if (argc > 1 && std::string(argv[1]) == "--timing-validation") return ValidateVsrTiming(argc, argv);
    TestPolicy();
    const bool nativeColor = argc > 1 && std::string(argv[1]) == "--bench-native-color";
    const bool native = nativeColor || (argc > 1 && std::string(argv[1]) == "--bench-native");
    const bool bench720To4k = argc > 1 && std::string(argv[1]) == "--bench-720p-4k";
    const bool bench720 = bench720To4k || (argc > 1 && std::string(argv[1]) == "--bench-720p");
    const bool benchFhdQhd = argc > 1 && std::string(argv[1]) == "--bench-fhd-qhd";
    const bool benchmark = native || bench720 || benchFhdQhd || (argc > 1 && std::string(argv[1]) == "--bench");
    bool yuy2 = false;
    int benchFps = 60;
    for (int i = 2; i < argc; ++i) {
        yuy2 |= std::string(argv[i]) == "--yuy2";
        if (std::string(argv[i]) == "--120") benchFps = 120;
    }
    const bool reverse = argc > 2 && std::string(argv[2]) == "--reverse";
    const bool hd720 = argc > 1 && std::string(argv[1]) == "--720p";
    Check(CoInitializeEx(nullptr, COINIT_MULTITHREADED), "COM");
    if (argc > 1 && std::string(argv[1]) == "--settings-controller") {
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        INITCOMMONCONTROLSEX controls{sizeof(controls),ICC_WIN95_CLASSES};
        Require(InitCommonControlsEx(&controls) != FALSE, "controller common controls");
        TestPlaybackPowerBroadcastRouting();
        TestSettingsController();
        TestExplicitCaptureRateSelection();
        TestFrameOptionsRedraw();
        TestSettingsShortcutStatusRefresh();
        TestSettingsAudioTransitions();
        TestSettingsBufferModeTransitions();
        TestSettingsSharedRefreshAcrossModes();
        TestSettingsAudioIdentityAndRefresh();
        TestExclusiveScanLifecycle();
        TestSettingsModeFlow();
        CoUninitialize();
        return 0;
    }
    if (argc > 1 && std::string(argv[1]) == "--probe") {
        const auto adapter = llcv::vsr::ProbeDefaultAdapter();
        Check(adapter.result, "default renderer adapter probe");
        Require(adapter.vendor != 0, "default adapter identifies a vendor");
        std::printf("Default adapter vendor=0x%04X (not an RTX capability check)\n", adapter.vendor);
        const auto mode = g_vsrMode.load();
        for (int i = 0; i < 3; ++i) {
            const auto result = llcv::vsr::ProbeSupport();
            std::printf("probe=%d state=%d HRESULT=0x%08X (global setting/activation unverified)\n",
                i, static_cast<int>(result.status), static_cast<unsigned>(result.result));
            Require(g_vsrMode.load() == mode, "probe never changes viewer VSR intent");
            Require(result.status != llcv::vsr::SupportProbe::Status::RequestSupported ||
                    result.result == S_OK, "request support must have exact success");
        }
        CoUninitialize();
        return 0;
    }
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    WNDCLASSW wc{}; wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"LLCV_VSR_TEST";
    Require(RegisterClassW(&wc) != 0, "test class");
    const int width = bench720 ? 1280 : benchmark ? 1920 : hd720 ? 1280 : 640;
    const int height = bench720 ? 720 : benchmark ? 1080 : hd720 ? 720 : 360;
    const int targetWidth = benchFhdQhd ? 2560 : native ? width : bench720To4k ? 3840 : hd720 ? 1920 : width*2;
    const int targetHeight = benchFhdQhd ? 1440 : native ? height : bench720To4k ? 2160 : hd720 ? 1080 : height*2;
    HWND window = CreateWindowW(wc.lpszClassName, L"Hidden VSR test", WS_POPUP,
        0,0,targetWidth,targetHeight,
        nullptr,nullptr,wc.hInstance,nullptr);
    Require(window != nullptr, "hidden window");
    if (benchmark) std::printf("BENCH input=%dx%d output=%dx%d fps=%d reverse=%d YUY2=%d; synthetic input, "
        "Smooth, no Present, driver quality unchanged/unverified\n",
        width,height,targetWidth,targetHeight,benchFps,reverse,yuy2);
    g_settings.pixelPerfect = false; g_settings.scalingMode = ScalingMode::Smooth;
    g_settings.presentationMode = PresentationMode::AllowTearing;
    g_suppressSettingsSave = true;
    std::vector<BYTE> pixels(static_cast<size_t>(width)*height*3/2, 128);
    if (nativeColor) for (int y = 0; y < height/2; ++y) for (int x = 0; x < width; x += 2) {
        const size_t offset = static_cast<size_t>(width)*height+static_cast<size_t>(y)*width+x;
        pixels[offset] = static_cast<BYTE>(96+((x/32+y/16)%5)*16);
        pixels[offset+1] = static_cast<BYTE>(96+((x/48+y/24)%5)*16);
    }
    for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x)
        pixels[static_cast<size_t>(y)*width+x] = static_cast<BYTE>(16 +
            ((x / 3 + y / 5 + ((x/40+y/40)%2)*90) % 220));
    // Prepare moving input before measuring. Generating/packing a full HD
    // pattern on every iteration can itself miss a 120fps deadline.
    std::array<std::vector<BYTE>,24> prepared;
    if (benchmark) for (size_t f = 0; f < prepared.size(); ++f) {
        auto nv12 = pixels;
        for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x)
            nv12[static_cast<size_t>(y)*width+x] = static_cast<BYTE>(16 +
                (((x+static_cast<int>(f)*3)/3+y/5+(((x+static_cast<int>(f)*3)/40+y/40)%2)*90)%220));
        if (!yuy2) { prepared[f] = std::move(nv12); continue; }
        auto& p = prepared[f]; p.resize(static_cast<size_t>(width)*height*2);
        for (int y = 0; y < height; ++y) for (int x = 0; x < width; x += 2) {
            const size_t out = (static_cast<size_t>(y)*width+x)*2;
            const size_t uv = static_cast<size_t>(width)*height+static_cast<size_t>(y/2)*width+x;
            p[out] = nv12[static_cast<size_t>(y)*width+x]; p[out+1] = nv12[uv];
            p[out+2] = nv12[static_cast<size_t>(y)*width+x+1]; p[out+3] = nv12[uv+1];
        }
    }
    const int phases = benchmark ? 6 : 3;
    uint64_t defaultHash = 0;
    std::vector<BYTE> firstOff, firstOn;
    for (int phase = 0; phase < phases; ++phase) {
        g_vsrMode = benchmark ? ((static_cast<bool>(phase % 2) != reverse) ? llcv::vsr::Mode::On : llcv::vsr::Mode::Off)
            : (phase == 0 ? llcv::vsr::Mode::Disabled : llcv::vsr::Mode::Off);
        DirectD3D11Renderer renderer;
        const HRESULT init = renderer.initialize(window,width,height,benchFps,
            yuy2 ? VideoPixelFormat::Yuy2 : VideoPixelFormat::Nv12);
        if (FAILED(init)) {
            std::printf("GPU/extension unavailable: 0x%08X (not a successful VSR test)\n", static_cast<unsigned>(init));
            return 77;
        }
        if (phase == 0 && !benchmark)
            Require(renderer.vsrState == llcv::vsr::State::Untouched, "default initialization untouched");
        if (benchmark && renderer.vsrState == llcv::vsr::State::Bypassed) {
            std::puts("SKIP: NVIDIA SDR native/upscale route not available"); return 77;
        }
        if (benchmark) {
            Require(renderer.vsrEligible, "benchmark uses production eligibility without overrides");
            Require(renderer.outputWidth == static_cast<UINT>(targetWidth) &&
                    renderer.outputHeight == static_cast<UINT>(targetHeight), "exact benchmark output size");
            Require(renderer.vsrState == (g_vsrMode == llcv::vsr::Mode::On
                    ? llcv::vsr::State::Requested : llcv::vsr::State::Off),
                    "benchmark requires accepted ON/OFF requests");
        }
        // CPU instrumentation is identical for A/B. Small bounded warmup in
        // synthetic bench; interactive app excludes five seconds of frames.
        // The production renderer carries no private timing histograms. Own the
        // synthetic benchmark's instrumentation here, outside its render path.
        llcv::vsr::Timing timing;
        if (benchmark) Check(timing.Initialize(renderer.device,60), "CPU timer");
        // Independent, serialized completion cross-check. This forced readback
        // exists ONLY in this executable, never in the viewer. Its overhead
        // means these timings must not be described as actual display latency.
        Microsoft::WRL::ComPtr<ID3D11Texture2D> completionPixel;
        llcv::vsr::Distribution completed;
        if (benchmark) {
            D3D11_TEXTURE2D_DESC desc{};
            desc.Width = desc.Height = desc.MipLevels = desc.ArraySize = 1;
            desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; desc.SampleDesc.Count = 1;
            desc.Usage = D3D11_USAGE_STAGING; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            Check(renderer.device->CreateTexture2D(&desc,nullptr,&completionPixel), "completion dependency");
        }
        const int frames = benchFhdQhd ? benchFps * 6 : (native || bench720) ? 360 : benchmark ? 240 : 1;
        const int warmup = benchFhdQhd ? benchFps * 2 : (native || bench720) ? 120 : 60;
        llcv::vsr::Distribution uploadTime, blitTime, readbackTime;
        uint64_t overBudget = 0;
        auto next = std::chrono::steady_clock::now();
        const auto phaseStart = next;
        for (int frame = 0; frame < frames; ++frame) {
            const auto& sourcePixels = benchmark ? prepared[frame%prepared.size()] : pixels;
            const auto completionStart = std::chrono::steady_clock::now();
            renderer.upload(sourcePixels.data(), static_cast<UINT32>(width*(yuy2 ? 2 : 1)));
            const auto uploadEnd = std::chrono::steady_clock::now();
            D3D11_VIDEO_PROCESSOR_STREAM stream{};
            stream.Enable = TRUE; stream.pInputSurface = renderer.inputViews[renderer.activeUploadSurface];
            timing.Begin(renderer.context);
            const HRESULT hr = renderer.videoContext->VideoProcessorBlt(renderer.processor,
                renderer.outputView,0,1,&stream);
            timing.End(renderer.context,SUCCEEDED(hr));
            Check(hr, "video processor blit");
            const auto blitEnd = std::chrono::steady_clock::now();
            // Test-only submission: no Present to a hidden window. Not an
            // input-to-display benchmark. The application uses normal Present.
            renderer.context->Flush();
            if (benchmark) {
                const D3D11_BOX pixelBox{0,0,0,1,1,1};
                renderer.context->CopySubresourceRegion(completionPixel.Get(),0,0,0,0,
                    renderer.backBuffer,0,&pixelBox);
                D3D11_MAPPED_SUBRESOURCE mapped{};
                Check(renderer.context->Map(completionPixel.Get(),0,D3D11_MAP_READ,0,&mapped), "serialized completion");
                renderer.context->Unmap(completionPixel.Get(),0);
                if (frame >= warmup) {
                    const auto end = std::chrono::steady_clock::now();
                    const double ms = std::chrono::duration<double,std::milli>(end-completionStart).count();
                    completed.Add(ms);
                    overBudget += ms > 1000.0/benchFps;
                    uploadTime.Add(std::chrono::duration<double,std::milli>(uploadEnd-completionStart).count());
                    blitTime.Add(std::chrono::duration<double,std::milli>(blitEnd-uploadEnd).count());
                    readbackTime.Add(std::chrono::duration<double,std::milli>(end-blitEnd).count());
                }
                next += std::chrono::microseconds(1000000/benchFps);
                std::this_thread::sleep_until(next);
            }
        }
        if (benchmark) std::printf("CADENCE frames=%d elapsed=%.3f s average=%.3f fps (serialized test only)\n",
            frames,std::chrono::duration<double>(std::chrono::steady_clock::now()-phaseStart).count(),
            frames/std::chrono::duration<double>(std::chrono::steady_clock::now()-phaseStart).count());
        const uint64_t hash = OutputHash(renderer); // blocks only after benchmark
        if (benchmark) {
            auto current = OutputPixels(renderer);
            auto& first = g_vsrMode == llcv::vsr::Mode::On ? firstOn : firstOff;
            if (first.empty()) first = current;
            else PrintPixelDifference(first,current,g_vsrMode == llcv::vsr::Mode::On ? "ON repeat" : "OFF repeat");
            if (!firstOn.empty() && !firstOff.empty()) PrintPixelDifference(firstOff,firstOn,"OFF vs ON");
        }
        if (!benchmark) {
            if (phase == 0) defaultHash = hash;
            else Require(hash == defaultHash, "explicit OFF/recreation equals original pixels");
        }
        const auto data = timing.Snapshot();
        std::printf("phase=%d requested=%s state=%d output_hash=%016llX CPU_Blt_n=%llu mean=%.4f ms\n", phase,
            g_vsrMode == llcv::vsr::Mode::On ? "on" : g_vsrMode == llcv::vsr::Mode::Off ? "off" : "default",
            static_cast<int>(renderer.vsrState),hash,data.cpuBlt.count,data.cpuBlt.Mean());
        if (benchmark) std::printf("phase=%d SERIALIZED upload+VP+readback n=%llu mean=%.4f "
            "p95_upper=%.2f p99_upper=%.2f max=%.4f ms (NOT display latency)\n",
            phase,completed.count,completed.Mean(),completed.Percentile(.95),
            completed.Percentile(.99),completed.maximum);
        if (benchmark) Require(completed.count > 100, "enough completion timing samples");
        if (benchmark) std::printf("DETAIL over_budget=%llu/%llu upload_mean=%.4f max=%.4f blit_mean=%.4f max=%.4f readback_mean=%.4f max=%.4f ms\n",
            overBudget,completed.count,uploadTime.Mean(),uploadTime.maximum,blitTime.Mean(),blitTime.maximum,
            readbackTime.Mean(),readbackTime.maximum);
    }
    if (native) TestNativeControls(window,width,height,pixels);
    if (!benchmark) {
        TestLiveToggle(window,width,height,pixels);
        std::vector<BYTE> yuyPixels(static_cast<size_t>(width)*height*2,128);
        for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x)
            yuyPixels[(static_cast<size_t>(y)*width+x)*2] = pixels[static_cast<size_t>(y)*width+x];
        TestLiveToggle(window,width,height,yuyPixels,VideoPixelFormat::Yuy2);
        TestCenteredPixelPerfect(window);
        TestSplitResolutionRenderer(window);
    }
    DestroyWindow(window); UnregisterClassW(wc.lpszClassName,wc.hInstance);
    CoUninitialize();
    std::puts(benchmark ? "Synthetic benchmark only; S_OK/hash changes do not certify RTX activation or display latency."
                        : "PASS: policy, default/OFF pixels, six live A/B pairs, F6 repeats, coalescing, HUD, occlusion and resize.");
    return 0;
}
