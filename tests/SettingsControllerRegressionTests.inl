// Included by VsrExperimentTests: hidden/offscreen HWNDs and injected workers only.
struct FrameOptionsRedrawProbe {
    bool observing = false;
    unsigned transitions = 0;
    unsigned unbatchedTransitions = 0;
    unsigned intermediatePaints = 0;
};
static LRESULT CALLBACK ObserveFrameOptionsRedraw(HWND hwnd, UINT message, WPARAM wParam,
                                                 LPARAM lParam, UINT_PTR, DWORD_PTR context) {
    auto& probe = *reinterpret_cast<FrameOptionsRedrawProbe*>(context);
    if (probe.observing) {
        if (message == WM_SHOWWINDOW || message == WM_ENABLE) {
            ++probe.transitions;
            if (IsWindowVisible(GetParent(hwnd))) ++probe.unbatchedTransitions;
        }
        if (message == WM_PAINT) ++probe.intermediatePaints;
    }
    return DefSubclassProc(hwnd, message, wParam, lParam);
}
static void TestFrameOptionsRedraw() {
    // Unlike hidden fixtures and the separately batched preview, exercise a
    // visible (offscreen, non-activating) production controller. A hidden owner
    // cannot reveal intermediate native help-button painting.
    WNDCLASSW wc{};
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpfnWndProc = SettingsControllerTestProc;
    wc.lpszClassName = L"LLCV_FRAME_OPTIONS_REDRAW_TEST";
    Require(RegisterClassW(&wc) != 0, "register flyout redraw fixture");
    SettingsDialogState state;
    state.activeTab = SettingsTab::VideoWindow;
    HWND hwnd = CreateWindowExW(WS_EX_NOACTIVATE, wc.lpszClassName, L"Offscreen flyout fixture",
        WS_POPUP | WS_CLIPCHILDREN, -32000, -32000, 1000, 650, nullptr, nullptr, wc.hInstance, &state);
    Require(hwnd != nullptr, "create flyout redraw fixture");
    const auto child = [&](const wchar_t* kind, DWORD style, int id) {
        return CreateWindowW(kind, L"?", WS_CHILD | style, 516, 420, 24, 24,
            hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), wc.hInstance, nullptr);
    };
    state.frameOptionsButton = child(L"BUTTON", BS_PUSHBUTTON, IDC_SETTINGS_FRAME_OPTIONS);
    state.frameOptionsClose = child(L"BUTTON", BS_PUSHBUTTON, IDC_SETTINGS_FRAME_OPTIONS_CLOSE);
    state.capture40Help = child(L"BUTTON", BS_PUSHBUTTON, IDC_SETTINGS_CAPTURE_40_HELP);
    state.sdrRangeHelp = child(L"BUTTON", BS_PUSHBUTTON, IDC_SETTINGS_SDR_RANGE_HELP);
    state.mjpegColorHelp = child(L"BUTTON", BS_PUSHBUTTON, IDC_SETTINGS_MJPEG_COLOR_HELP);
    state.forceHdr10Help = child(L"BUTTON", BS_PUSHBUTTON, IDC_SETTINGS_FORCE_HDR10_HELP);
    state.pixelFormatCombo = child(L"COMBOBOX", CBS_DROPDOWNLIST, IDC_SETTINGS_PIXEL_FORMAT);
    const VideoPixelFormat formats[] = {VideoPixelFormat::Nv12, VideoPixelFormat::Mjpeg, VideoPixelFormat::P010};
    for (const auto format : formats) {
        const auto row = SendMessageW(state.pixelFormatCombo, CB_ADDSTRING, 0,
            reinterpret_cast<LPARAM>(PixelFormatName(format)));
        SendMessageW(state.pixelFormatCombo, CB_SETITEMDATA, row, static_cast<LPARAM>(format));
    }
    Require(state.theme.Attach(hwnd, &state), "attach real theme for flyout redraw regression");
    FrameOptionsRedrawProbe probe;
    const HWND watched[] = {state.capture40Help, state.sdrRangeHelp, state.mjpegColorHelp, state.forceHdr10Help};
    for (HWND control : watched)
        Require(SetWindowSubclass(control, ObserveFrameOptionsRedraw, 74,
            reinterpret_cast<DWORD_PTR>(&probe)), "watch flyout help-button transitions");
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    for (int format = 0; format < 3; ++format) {
        SendMessageW(state.pixelFormatCombo, CB_SETCURSEL, format, 0);
        UpdateAdvancedControlVisibility(&state);
        RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW);
        for (int cycle = 0; cycle < 20; ++cycle) {
            const auto before = probe.transitions;
            probe.observing = true;
            SendMessageW(hwnd, WM_COMMAND, MAKEWPARAM(IDC_SETTINGS_FRAME_OPTIONS, BN_CLICKED), 0);
            probe.observing = false;
            Require(state.frameOptionsOpen && IsWindowVisible(hwnd) && IsWindowVisible(state.capture40Help) &&
                    IsWindowEnabled(state.capture40Help), "flyout opens with a fully enabled visible help button");
            Require(probe.transitions > before && !probe.unbatchedTransitions && !probe.intermediatePaints,
                "production flyout batches every help-button show/hide/enable without intermediate paints");
            Require(GetUpdateRect(state.capture40Help, nullptr, FALSE), "flyout help receives its final repaint");
            UpdateWindow(state.capture40Help);
            Require(!GetUpdateRect(state.capture40Help, nullptr, FALSE), "final help paint is consumed normally");
            probe.observing = true;
            SendMessageW(hwnd, WM_COMMAND, MAKEWPARAM(IDC_SETTINGS_FRAME_OPTIONS_CLOSE, BN_CLICKED), 0);
            probe.observing = false;
            Require(!state.frameOptionsOpen && IsWindowVisible(hwnd) && !IsWindowVisible(state.capture40Help) &&
                    !probe.unbatchedTransitions && !probe.intermediatePaints,
                "flyout close also batches restored format controls without hiding the owner");
            Require(SendMessageW(state.pixelFormatCombo, CB_GETCURSEL, 0, 0) == format,
                "repeated flyout transitions preserve the pending pixel format");
        }
    }
    ShowWindow(hwnd, SW_HIDE);
    for (int cycle = 0; cycle < 2; ++cycle) {
        SendMessageW(hwnd, WM_COMMAND, MAKEWPARAM(IDC_SETTINGS_FRAME_OPTIONS, BN_CLICKED), 0);
        Require(!IsWindowVisible(hwnd), "flyout transitions never reveal a hidden owner");
    }
    Require(!state.videoModeCache && !state.captureAudioProbeCache && !state.sharedProbeCache,
        "flyout redraw never starts hardware or audio probing");
    for (HWND control : watched) RemoveWindowSubclass(control, ObserveFrameOptionsRedraw, 74);
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    std::puts("Flyout redraw: 60 production open/close cycles across SDR, MJPEG and HDR passed.");
}
static void TestExplicitCaptureRateSelection() {
    const auto saved = g_settings;
    const bool savedSuppress = g_suppressSettingsSave;
    g_settings = {};
    g_settings.videoFrameRate = 60;
    g_suppressSettingsSave = true;
    WNDCLASSW wc{};
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpfnWndProc = SettingsControllerTestProc;
    wc.lpszClassName = L"LLCV_EXPLICIT_CAPTURE_RATE_TEST";
    Require(RegisterClassW(&wc) != 0, "register explicit FPS fixture");
    SettingsDialogState state;
    state.activeTab = SettingsTab::VideoWindow;
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"Hidden FPS fixture", WS_POPUP,
        -32000, -32000, 1000, 650, nullptr, nullptr, wc.hInstance, &state);
    const auto child = [&](const wchar_t* kind, DWORD style, int id) {
        return CreateWindowW(kind, L"", WS_CHILD | style, 0, 0, 420, 30,
            hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), wc.hInstance, nullptr);
    };
    state.tabControl = child(L"LISTBOX", LBS_NOTIFY, IDC_SETTINGS_TAB);
    for (int i = 0; i < llcv::settings_ui::kSettingsNavigationCount; ++i)
        SendMessageW(state.tabControl, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Page"));
    state.sectionNavigation = child(L"COMBOBOX", CBS_DROPDOWNLIST, IDC_SETTINGS_SECTION);
    state.pixelFormatCombo = child(L"COMBOBOX", CBS_DROPDOWNLIST, IDC_SETTINGS_PIXEL_FORMAT);
    state.frameRateCombo = child(L"COMBOBOX", CBS_DROPDOWNLIST, IDC_SETTINGS_FRAME_RATE);
    state.startButton = child(L"BUTTON", BS_PUSHBUTTON, IDC_SETTINGS_START);
    state.audioOnlyCheck = child(L"BUTTON", BS_AUTOCHECKBOX, IDC_SETTINGS_AUDIO_ONLY);
    state.capture40Check = child(L"BUTTON", BS_AUTOCHECKBOX, IDC_SETTINGS_CAPTURE_40);
    state.unlistedFpsCheck = child(L"BUTTON", BS_AUTOCHECKBOX, IDC_SETTINGS_UNLISTED_FPS);
    state.frameOptionsButton = child(L"BUTTON", BS_PUSHBUTTON, IDC_SETTINGS_FRAME_OPTIONS);
    state.frameOptionsClose = child(L"BUTTON", BS_PUSHBUTTON, IDC_SETTINGS_FRAME_OPTIONS_CLOSE);
    state.videoCapabilityStatus = child(L"STATIC", SS_LEFT, 0);
    state.unlistedFpsStatus = child(L"STATIC", SS_LEFT, 0);
    for (auto format : {VideoPixelFormat::Auto, VideoPixelFormat::Nv12, VideoPixelFormat::Yuy2,
                        VideoPixelFormat::P010, VideoPixelFormat::Mjpeg}) {
        const auto row = SendMessageW(state.pixelFormatCombo, CB_ADDSTRING, 0,
            reinterpret_cast<LPARAM>(PixelFormatName(format)));
        SendMessageW(state.pixelFormatCombo, CB_SETITEMDATA, row, static_cast<LPARAM>(format));
    }
    const auto refresh = [&] {
        PopulateFrameRateCombo(&state);
        UpdateVideoCapabilityStatus(&state);
        UpdateAdvancedControlVisibility(&state);
    };
    // The parent intentionally stays hidden: IsWindowVisible would always be
    // false, so inspect the child style to verify actual show/hide decisions.
    const auto warningVisible = [&] {
        return (GetWindowLongPtrW(state.unlistedFpsStatus, GWL_STYLE) & WS_VISIBLE) != 0;
    };
    const auto selectTab = [&](SettingsTab tab) {
        SendMessageW(state.tabControl, LB_SETCURSEL,
            llcv::settings_ui::SettingsNavigationIndex(tab), 0);
        SendMessageW(hwnd, WM_COMMAND, MAKEWPARAM(IDC_SETTINGS_TAB, LBN_SELCHANGE),
            reinterpret_cast<LPARAM>(state.tabControl));
        Require(state.activeTab == tab && IsWindow(hwnd) && !IsWindowVisible(hwnd),
            "FPS warning fixture uses the real tab handler without showing a window");
    };
    const auto chooseRate = [&](int fps) {
        for (LRESULT row = 0; row < SendMessageW(state.frameRateCombo, CB_GETCOUNT, 0, 0); ++row) {
            if (SendMessageW(state.frameRateCombo, CB_GETITEMDATA, row, 0) != fps) continue;
            SendMessageW(state.frameRateCombo, CB_SETCURSEL, row, 0);
            SendMessageW(hwnd, WM_COMMAND, MAKEWPARAM(IDC_SETTINGS_FRAME_RATE, CBN_SELCHANGE), 0);
            return;
        }
        Require(false, "requested fixture FPS row exists");
    };
    SendMessageW(state.pixelFormatCombo, CB_SETCURSEL, 0, 0);
    state.pixelFormats = {{VideoPixelFormat::Nv12, 120}, {VideoPixelFormat::Yuy2, 60}};
    refresh();
    Require(SelectedCaptureFrameRate(&state) == 60 && SettingsCanStart(&state),
        "explicit 60 survives automatic format union with faster NV12");
    SendMessageW(state.pixelFormatCombo, CB_SETCURSEL, 1, 0);
    refresh();
    Require(SelectedCaptureFrameRate(&state) == 60 && !SettingsCanStart(&state) &&
            !IsWindowEnabled(state.startButton) && GetWindowTextLengthW(state.unlistedFpsStatus) > 0 &&
            warningVisible(),
        "incompatible format preserves explicit FPS and explains disabled Start");
    EnableWindow(state.startButton, TRUE);
    FinishSettingsDialog(hwnd, &state, true);
    Require(IsWindow(hwnd) && !state.accepted && g_settings.videoFrameRate == 60,
        "stale enabled Start cannot save unavailable FPS as Auto");
    chooseRate(120);
    Require(SettingsCanStart(&state) && IsWindowEnabled(state.startButton) &&
            GetWindowTextLengthW(state.unlistedFpsStatus) == 0 && !warningVisible(),
        "valid FPS re-enables Start and immediately hides the cleared warning");
    selectTab(SettingsTab::Audio);
    selectTab(SettingsTab::VideoWindow);
    Require(!warningVisible(), "cleared FPS warning remains hidden after a tab roundtrip");
    chooseRate(60);
    Require(!SettingsCanStart(&state) && !IsWindowEnabled(state.startButton) &&
            GetWindowTextLengthW(state.unlistedFpsStatus) > 0 && warningVisible(),
        "retained unavailable FPS shows its warning again after a tab roundtrip");
    chooseRate(120);

    // A compact page only lays out nonempty warnings. Reintroducing a warning
    // must refresh both its page mask and its bounds, not merely its text.
    llcv::settings_ui::FitSettingsClientArea(&state, SIZE{760, 520}, 96);
    state.layoutDpi = 96;
    UpdateAdvancedControlVisibility(&state);
    const auto formatPage = std::find(state.compactPageNames.begin(), state.compactPageNames.end(),
        state.english ? L"Video format" : L"영상 형식");
    Require(formatPage != state.compactPageNames.end(), "compact video format page exists");
    SendMessageW(state.sectionNavigation, CB_SETCURSEL,
        static_cast<WPARAM>(formatPage - state.compactPageNames.begin()), 0);
    SendMessageW(hwnd, WM_COMMAND, MAKEWPARAM(IDC_SETTINGS_SECTION, CBN_SELCHANGE),
        reinterpret_cast<LPARAM>(state.sectionNavigation));
    const auto requireCompactWarning = [&] {
        RECT warning{}, capability{};
        GetWindowRect(state.unlistedFpsStatus, &warning);
        GetWindowRect(state.videoCapabilityStatus, &capability);
        MapWindowPoints(nullptr, hwnd, reinterpret_cast<POINT*>(&warning), 2);
        MapWindowPoints(nullptr, hwnd, reinterpret_cast<POINT*>(&capability), 2);
        Require(!SettingsCanStart(&state) && !IsWindowEnabled(state.startButton) &&
                GetWindowTextLengthW(state.unlistedFpsStatus) > 0 && warningVisible() &&
                llcv::settings_ui::SettingsControlOnCurrentPage(state.unlistedFpsStatus),
            "compact unavailable FPS has a visible warning on the current page");
        Require(warning.left == capability.left && warning.right == capability.right &&
                warning.top >= capability.bottom && warning.bottom > warning.top &&
                warning.left >= state.sidebarWidthDip + 20 &&
                warning.right <= state.clientWidthDip - 24 && warning.bottom <= state.footerTopDip - 16,
            "compact FPS warning stays aligned below capabilities and above the footer");
    };
    chooseRate(60);
    requireCompactWarning();
    chooseRate(120);
    Require(SettingsCanStart(&state) && IsWindowEnabled(state.startButton) &&
            GetWindowTextLengthW(state.unlistedFpsStatus) == 0 && !warningVisible() &&
            !llcv::settings_ui::SettingsControlOnCurrentPage(state.unlistedFpsStatus),
        "valid compact FPS removes the empty warning from both visibility and page layout");
    selectTab(SettingsTab::Audio);
    selectTab(SettingsTab::VideoWindow);
    chooseRate(60);
    requireCompactWarning();
    chooseRate(120);
    llcv::settings_ui::FitSettingsClientArea(&state, SIZE{1000, 650}, 96);
    UpdateAdvancedControlVisibility(&state);
    state.pixelFormats.clear(); state.videoCapabilityQueryStatus = E_FAIL;
    refresh();
    Require(SelectedCaptureFrameRate(&state) == 120 && !SettingsCanStart(&state),
        "failed capability refresh preserves the working FPS");
    state.pixelFormats = {{VideoPixelFormat::Nv12, 120}}; state.videoCapabilityQueryStatus = S_OK;
    refresh();
    Require(SelectedCaptureFrameRate(&state) == 120 && SettingsCanStart(&state),
        "successful refresh restores the prior FPS without changing it");
    SendMessageW(state.unlistedFpsCheck, BM_SETCHECK, BST_CHECKED, 0);
    refresh(); chooseRate(30);
    Require(SettingsCanStart(&state), "explicit unlisted 30 is enabled only by opt-in");
    SendMessageW(state.unlistedFpsCheck, BM_SETCHECK, BST_UNCHECKED, 0);
    refresh();
    Require(SelectedCaptureFrameRate(&state) == 30 && !SettingsCanStart(&state),
        "turning off unlisted requests does not silently select faster Auto");
    SendMessageW(state.capture40Check, BM_SETCHECK, BST_CHECKED, 0);
    UpdateVideoCapabilityStatus(&state);
    Require(SettingsCanStart(&state) && SelectedCaptureFrameRate(&state) == 30,
        "40fps overrides without erasing the normal FPS selection");
    SendMessageW(state.capture40Check, BM_SETCHECK, BST_UNCHECKED, 0);
    UpdateVideoCapabilityStatus(&state);
    Require(!SettingsCanStart(&state), "leaving 40fps restores the explicit unavailable rate guard");
    // Exercise production routing, not a standalone view approximation.
    // Closing the flyout via Escape must not cancel/destroy the settings dialog.
    const auto persistedRate = g_settings.videoFrameRate;
    SendMessageW(hwnd,WM_COMMAND,MAKEWPARAM(IDC_SETTINGS_FRAME_OPTIONS,BN_CLICKED),0);
    Require(state.frameOptionsOpen,"controller opens frame options");
    SendMessageW(state.unlistedFpsCheck,BM_SETCHECK,BST_CHECKED,0);
    SendMessageW(hwnd,WM_COMMAND,MAKEWPARAM(IDC_SETTINGS_UNLISTED_FPS,BN_CLICKED),0);
    Require(state.frameOptionsOpen && SelectedCaptureFrameRate(&state)==30 && SettingsCanStart(&state),
        "flyout opt-in uses existing FPS negotiation and preserves selected 30");
    SendMessageW(hwnd,WM_COMMAND,IDCANCEL,0);
    Require(IsWindow(hwnd) && !state.frameOptionsOpen && SelectedCaptureFrameRate(&state)==30 &&
            SendMessageW(state.unlistedFpsCheck,BM_GETCHECK,0,0)==BST_CHECKED &&
            g_settings.videoFrameRate==persistedRate,
        "Escape dismisses flyout without committing or discarding pending capture settings");
    SendMessageW(hwnd,WM_COMMAND,MAKEWPARAM(IDC_SETTINGS_FRAME_OPTIONS,BN_CLICKED),0);
    SendMessageW(hwnd,WM_ACTIVATE,WA_INACTIVE,0);
    Require(!state.frameOptionsOpen && IsWindow(hwnd),"Alt+Tab dismisses flyout without closing settings");
    SendMessageW(state.unlistedFpsCheck,BM_SETCHECK,BST_UNCHECKED,0);
    refresh();
    chooseRate(0);
    for (const auto format : {VideoPixelFormat::P010, VideoPixelFormat::Mjpeg}) {
        SendMessageW(state.pixelFormatCombo, CB_SETCURSEL, 0, 0);
        state.pixelFormats = {{format, 60}};
        refresh();
        Require(!SettingsCanStart(&state), "Auto must not enable unsupported HDR/compressed-only selection");
        SendMessageW(state.pixelFormatCombo, CB_SETCURSEL, format == VideoPixelFormat::P010 ? 3 : 4, 0);
        refresh();
        Require(SettingsCanStart(&state), "explicit HDR/compressed formats remain available");
    }
    state.videoModesPending = true;
    Require(!SettingsCanStart(&state), "pending refresh cannot start stale video settings");
    SendMessageW(state.audioOnlyCheck, BM_SETCHECK, BST_CHECKED, 0);
    Require(SettingsCanStart(&state), "audio-only remains independent of video rate availability");
    for (int fps : {30, 60, 120, 144, 240}) {
        g_settings.videoFrameRate = fps;
        Require(RequestsExactCaptureRate() && RequestedVideoFrameRate() == fps && !AllowsUnlistedCaptureRate(),
            "every explicit FPS is strict without implicit direct requests");
    }
    g_settings.allowUnlistedFps = true; g_settings.videoFrameRate = 30;
    Require(AllowsUnlistedCaptureRate(), "30fps direct requests require explicit permission");
    g_settings.videoFrameRate = 60;
    Require(!AllowsUnlistedCaptureRate(), "unlisted opt-in never forces arbitrary unsupported rates");
    g_settings.videoFrameRate = 0;
    Require(!RequestsExactCaptureRate(), "Auto keeps automatic mode selection");
    Require(!state.videoModeCache && !state.captureAudioProbeCache && !state.sharedProbeCache,
        "FPS edits never initiate hardware or output probes");
    // Exercise the real asynchronous format-list rebuild too, not only the
    // rate list. A missing HDR format must not silently become SDR Auto.
    state.videoCombo = child(L"COMBOBOX", CBS_DROPDOWNLIST, IDC_SETTINGS_VIDEO);
    for (const auto& preset : kVideoPresets)
        SendMessageW(state.videoCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(preset.label));
    SendMessageW(state.videoCombo, CB_SETCURSEL, 1, 0);
    SendMessageW(state.audioOnlyCheck, BM_SETCHECK, BST_UNCHECKED, 0);
    SendMessageW(state.pixelFormatCombo, CB_SETCURSEL, 3, 0);
    SendMessageW(state.frameRateCombo, CB_SETCURSEL, static_cast<WPARAM>(-1), 0);
    g_settings.videoFrameRate = 60;
    std::atomic<int> queryPhase{0};
    state.videoModeCache = std::make_unique<llcv::settings_ui::SettingsModeCache<SettingsVideoModes>>(
        [&](const auto&) {
            SettingsVideoModes modes;
            const int phase = queryPhase.load();
            if (phase == 2) modes.status = E_FAIL;
            else modes.formats = {{phase == 1 || phase == 4 ? VideoPixelFormat::Nv12 : VideoPixelFormat::P010, 60}};
            return modes;
        }, [] {});
    const auto query = [&](int phase) {
        queryPhase = phase;
        state.videoModeCache->Invalidate();
        state.appliedVideoModeKey.reset();
        const auto deadline = GetTickCount64() + 2000;
        do {
            PopulatePixelFormatCombo(&state);
            if (state.videoModesPending) Sleep(1);
        } while (state.videoModesPending && GetTickCount64() < deadline);
        Require(!state.videoModesPending, "synthetic video-mode query completes");
    };
    for (int phase : {0, 1, 2, 3}) {
        query(phase);
        Require(SelectedPixelFormat(&state) == VideoPixelFormat::P010 && SelectedCaptureFrameRate(&state) == 60,
            "format rebuild/query failure never replaces explicit P010 or 60fps");
        Require(SettingsCanStart(&state) == (phase == 0 || phase == 3),
            "missing format blocks startup and recovered format restores it");
    }
    query(4);
    for (LRESULT row = 0; row < SendMessageW(state.pixelFormatCombo, CB_GETCOUNT, 0, 0); ++row) {
        if (SendMessageW(state.pixelFormatCombo, CB_GETITEMDATA, row, 0) == static_cast<LPARAM>(VideoPixelFormat::Nv12))
            SendMessageW(state.pixelFormatCombo, CB_SETCURSEL, row, 0);
    }
    refresh();
    Require(SettingsCanStart(&state), "explicit user format change restores startup");
    query(4);
    Require(SelectedPixelFormat(&state) == VideoPixelFormat::Nv12 && SelectedCaptureFrameRate(&state) == 60 && SettingsCanStart(&state),
        "later refresh retains the user's new format and FPS");
    state.videoModeCache.reset();
    DestroyWindow(hwnd); UnregisterClassW(wc.lpszClassName, wc.hInstance);
    g_settings = saved; g_suppressSettingsSave = savedSuppress;
    std::puts("Explicit capture FPS: preservation, invalid-choice gate, opt-in, format availability and Auto passed.");
}

#ifdef LLCV_RECORDING
static void TestOrdinarySettingsRecordingNavigation() {
    const auto saved = g_settings;
    const bool savedSuppress = g_suppressSettingsSave;
    const int savedRequestedTab = g_requestedSettingsTab;
    const auto savedRecordingOptions = llcv::recording::sessionOptions;
    const bool savedRestart = g_restartToSettings.load();
    g_suppressSettingsSave = true;
    g_requestedSettingsTab = -1;
    WNDCLASSW wc{};
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpfnWndProc = SettingsControllerTestProc;
    wc.lpszClassName = L"LLCV_SETTINGS_RECORDING_NAVIGATION_TEST";
    Require(RegisterClassW(&wc) != 0, "register recording navigation fixture");
    wchar_t temporary[MAX_PATH]{};
    Require(GetTempPathW(ARRAYSIZE(temporary), temporary) != 0, "recording fixture storage path");
    for (const bool audioOnly : {false, true}) {
        g_settings = {};
        g_settings.audioOnly = audioOnly;
        g_settings.shortcuts[static_cast<size_t>(keys::Action::Settings)] = keys::Ctrl | 'K';
        const auto originalTab = audioOnly ? SettingsTab::Audio : SettingsTab::VideoWindow;
        SettingsDialogState state;
        state.activeTab = originalTab;
        // Supply completed capabilities: navigation must not launch a real encoder probe.
        state.recordingProbePending = false;
        state.recordingCapabilities[0] = {true, true, {{L"Fixture", L"test"}}, {true, true}, {true, true}};
        state.recordingOptions.folder = temporary;
        state.recordingOptions.maxHeight = 720;
        state.recordingOptions.maxFps = 30;
        HWND hwnd = CreateWindowW(wc.lpszClassName, L"Hidden recording navigation fixture", WS_POPUP,
            -32000, -32000, 1000, 650, nullptr, nullptr, wc.hInstance, &state);
        Require(hwnd != nullptr && !IsWindowVisible(hwnd), "ordinary settings fixture remains hidden");
        const llcv::settings_ui::SettingsControlInitialValues initial{
            g_settings, false, false, kAppVersionLabel, VideoPreset::R1920x1080,
            {}, {}, kVideoPresets, kPcmQueueOptionsMs, {}};
        const llcv::settings_ui::SettingsControlPopulation population{
            &state, [](void*) {}, [](void*) {}, [](void*) {}};
        llcv::settings_ui::CreateSettingsDialogControls(&state, hwnd, wc.hInstance, initial, population);
        llcv::settings_ui::CreateRecordingSettings(&state, hwnd);
        // Unaccepted edits on both the ordinary pages and the shortcut page
        // must survive opening Recording without saving or recreating the HWND.
        SendMessageW(state.videoCombo, CB_SETCURSEL, 2, 0);
        SendMessageW(state.pcmQueueCombo, CB_SETCURSEL, 1, 0);
        SendMessageW(state.pixelCheck, BM_SETCHECK, BST_UNCHECKED, 0);
        state.shortcutBindings[static_cast<size_t>(keys::Action::Settings)] = keys::Ctrl | 'J';
        llcv::settings_ui::RefreshShortcutCaptions(&state);
        const auto editedBindings = state.shortcutBindings;
        auto expectedOptions = state.recordingOptions;
        const auto selectTab = [&](SettingsTab tab) {
            SendMessageW(state.tabControl, LB_SETCURSEL,
                llcv::settings_ui::SettingsNavigationIndex(tab), 0);
            SendMessageW(hwnd, WM_COMMAND, MAKEWPARAM(IDC_SETTINGS_TAB, LBN_SELCHANGE),
                reinterpret_cast<LPARAM>(state.tabControl));
            Require(IsWindow(hwnd) && !IsWindowVisible(hwnd) && state.activeTab == tab && !state.recordingLive,
                "ordinary settings navigate in the same hidden non-live dialog");
            Require(state.recordingOptions == expectedOptions && state.shortcutBindings == editedBindings &&
                    SendMessageW(state.videoCombo, CB_GETCURSEL, 0, 0) == 2 &&
                    SendMessageW(state.pcmQueueCombo, CB_GETCURSEL, 0, 0) == 1 &&
                    SendMessageW(state.pixelCheck, BM_GETCHECK, 0, 0) == BST_UNCHECKED,
                "recording navigation preserves all pending recording/playback/shortcut edits");
            Require(g_requestedSettingsTab == -1 && g_restartToSettings.load() == savedRestart,
                "ordinary tab selection does not request a process restart");
        };
        selectTab(SettingsTab::Recording);
        Require(IsWindowEnabled(state.recVolume) && !llcv::settings_ui::CanToggleRecordingSettings(&state),
            "ordinary recording tab permits options without becoming live recording controls");
        // Exercise the real recording control command, not just the state model.
        SendMessageW(state.recVolume, CB_SETCURSEL, 6, 0);
        SendMessageW(hwnd, WM_COMMAND,
            MAKEWPARAM(llcv::settings_ui::kRecordingVolume, CBN_SELCHANGE),
            reinterpret_cast<LPARAM>(state.recVolume));
        expectedOptions.audioVolumePercent = 60;
        Require(state.recordingOptions == expectedOptions, "recording option is editable through the settings tab");
        selectTab(originalTab);
        selectTab(SettingsTab::Recording);
        Require(SendMessageW(state.recVolume, CB_GETCURSEL, 0, 0) == 6,
            "recording volume selection survives leaving and reopening its tab");
        selectTab(originalTab);
        wchar_t shortcutCaption[64]{};
        GetWindowTextW(state.guideKeys[static_cast<size_t>(keys::Action::Settings)], shortcutCaption, ARRAYSIZE(shortcutCaption));
        Require(std::wstring(shortcutCaption) == L"Ctrl+J" &&
                g_settings.shortcuts[static_cast<size_t>(keys::Action::Settings)] == (keys::Ctrl | 'K') &&
                llcv::recording::sessionOptions == savedRecordingOptions,
            "tab navigation retains the edited shortcut caption without committing settings");
        Require(!state.recordingProbe && !state.videoModeCache && !state.captureAudioProbeCache &&
                !state.sharedProbeCache && !state.inventoryCache && !state.exclusiveScanRunning,
            "fixture tab navigation uses supplied capabilities and never opens hardware");
        DestroyWindow(hwnd);
    }
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    g_settings = saved;
    g_suppressSettingsSave = savedSuppress;
    g_requestedSettingsTab = savedRequestedTab;
    std::puts("Settings navigation: Video/Audio to Recording round trips preserve pending options and shortcuts without a live viewer.");
}
#endif

static void TestSettingsShortcutStatusRefresh() {
#ifdef LLCV_RECORDING
    TestOrdinarySettingsRecordingNavigation();
#endif
    const auto saved=g_settings;
    const bool savedSuppress=g_suppressSettingsSave;
    g_suppressSettingsSave=true;
    WNDCLASSW wc{}; wc.hInstance=GetModuleHandleW(nullptr);
    wc.lpfnWndProc=SettingsControllerTestProc; wc.lpszClassName=L"LLCV_SHORTCUT_STATUS_TEST";
    Require(RegisterClassW(&wc)!=0,"register shortcut status fixture");
    SettingsDialogState state; state.asioAvailable=true;
    HWND hwnd=CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE,wc.lpszClassName,L"Shortcut status fixture",
        WS_POPUP|WS_VISIBLE,-32000,-32000,800,600,nullptr,nullptr,wc.hInstance,&state);
    Require(hwnd!=nullptr,"shortcut status owner");
    const auto child=[&](const wchar_t* name,DWORD style) {
        HWND value=CreateWindowW(name,L"",WS_CHILD|WS_VISIBLE|style,0,0,350,40,hwnd,nullptr,wc.hInstance,nullptr);
        Require(value!=nullptr,"shortcut status control"); return value;
    };
    state.audioCombo=child(L"COMBOBOX",CBS_DROPDOWNLIST);
    for(const auto* mode:{L"Shared",L"Exclusive",L"ASIO"}) SendMessageW(state.audioCombo,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(mode));
    SendMessageW(state.audioCombo,CB_SETCURSEL,0,0);
    state.surround51Check=child(L"BUTTON",BS_AUTOCHECKBOX);
    SendMessageW(state.surround51Check,BM_SETCHECK,BST_CHECKED,0);
    state.audioStatus=child(L"STATIC",SS_LEFT);
    state.guideVideoHint=child(L"STATIC",SS_LEFT);
    for(auto& key:state.guideKeys) key=child(L"BUTTON",BS_PUSHBUTTON);
    const auto text=[&] { wchar_t value[512]{}; GetWindowTextW(state.audioStatus,value,512); return std::wstring(value); };
    const auto edit=[&](UINT key) {
        llcv::settings_ui::BeginShortcutEdit(&state,static_cast<size_t>(keys::Action::Diagnostics));
        MSG input{}; input.hwnd=GetFocus(); input.message=WM_KEYDOWN; input.wParam=key;
        Require(ProcessSettingsShortcutEdit(&state,input,keys::Ctrl),"controller consumes shortcut assignment");
        input.message=WM_KEYUP;
        Require(ProcessSettingsShortcutEdit(&state,input,0),"controller consumes assignment key release");
    };
    UpdateAudioClient3Status(&state);
    Require(text().find(L"Tab")!=std::wstring::npos,"5.1 starts with default diagnostic key");
    edit('D');
    Require(text().find(L"Ctrl+D")!=std::wstring::npos && text().find(L"Tab")==std::wstring::npos,
        "5.1 diagnostic hint updates immediately on assignment");
    SendMessageW(hwnd,WM_COMMAND,MAKEWPARAM(IDC_SETTINGS_SHORTCUT_RESET,BN_CLICKED),0);
    Require(state.shortcutBindings==keys::Defaults && text().find(L"Tab")!=std::wstring::npos &&
        text().find(L"Ctrl+D")==std::wstring::npos,"reset updates both binding and 5.1 hint");
    for(int mode:{1,2,0}) {
        SendMessageW(state.audioCombo,CB_SETCURSEL,mode,0);
        SendMessageW(state.surround51Check,BM_SETCHECK,BST_UNCHECKED,0);
        SetWindowTextW(state.audioStatus,L"Exclusive result must survive");
        UpdateAudioClient3Status(&state);
        const auto previous=text();
        edit('K');
        Require(text()==previous,"shortcut update preserves non-5.1 and Exclusive status");
        SendMessageW(hwnd,WM_COMMAND,MAKEWPARAM(IDC_SETTINGS_SHORTCUT_RESET,BN_CLICKED),0);
        Require(text()==previous,"shortcut reset preserves non-5.1 and Exclusive status");
    }
    Require(!state.sharedProbeCache && !state.exclusiveScanRunning,"editing never starts hardware probes");
    DestroyWindow(hwnd); UnregisterClassW(wc.lpszClassName,wc.hInstance);
    g_settings=saved; g_suppressSettingsSave=savedSuppress;
    std::puts("Shortcut status: immediate edit/reset refresh and preserved audio modes passed.");
}

static void TestSettingsBufferModeTransitions() {
    const auto saved = g_settings;
    const bool savedSuppress = g_suppressSettingsSave;
    const bool savedFallback = g_exclusiveStartupFallback;
    g_suppressSettingsSave = true;
    g_settings.audioMode = AudioMode::WasapiExclusive;
    g_settings.audioOnly = true;
    g_settings.wasapiBufferMs = 20;
    g_settings.wasapiSharedPeriodFrames = 960;
    g_settings.audioOutputDeviceId = L"buffer-fixture";
    g_settings.asioDriverName = L"Buffer fixture ASIO";
    WNDCLASSW wc{};
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpfnWndProc = SettingsControllerTestProc;
    wc.lpszClassName = L"LLCV_SETTINGS_BUFFER_TEST";
    Require(RegisterClassW(&wc) != 0, "register buffer fixture");
    SettingsDialogState state;
    state.activeTab = SettingsTab::Audio;
    state.asioAvailable = true;
    state.asioDrivers = {{0, "Buffer fixture ASIO"}};
    state.audioEndpoints = {{L"buffer-fixture", L"Buffer fixture", true}};
    state.exclusiveEndpointResults.resize(1);
    state.exclusiveEndpointResults[0].state = ExclusiveEndpointState::Supported;
    state.exclusiveEndpointResults[0].recommendedBufferMs = 5;
    state.exclusiveScanCompleted = 1;
    state.probeReady = true;
    state.appliedSharedEndpoint = L"buffer-fixture";
    state.probe = {true, S_OK, 480, 48, 96, 1920, 0.0};
    state.selectedBufferMs = 20;
    state.selectedSharedPeriodFrames = 960;
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"Hidden buffer fixture", WS_POPUP,
        -32000, -32000, 1000, 650, nullptr, nullptr, wc.hInstance, &state);
    Require(hwnd != nullptr, "create buffer fixture");
    const llcv::settings_ui::SettingsControlInitialValues initial{
        g_settings, false, true, kAppVersionLabel, VideoPreset::R1920x1080,
        {}, {}, kVideoPresets, kPcmQueueOptionsMs, {}};
    const llcv::settings_ui::SettingsControlPopulation population{
        &state, [](void* s) { PopulateAudioOutputCombo(static_cast<SettingsDialogState*>(s)); },
        [](void* s) { PopulateSettingsBufferCombo(static_cast<SettingsDialogState*>(s)); },
        [](void*) {}};
    llcv::settings_ui::CreateSettingsDialogControls(&state, hwnd, wc.hInstance, initial, population);
    auto mode = [&](int selection) {
        SendMessageW(state.audioCombo, CB_SETCURSEL, selection, 0);
        SendMessageW(hwnd, WM_COMMAND, MAKEWPARAM(IDC_SETTINGS_AUDIO, CBN_SELCHANGE), 0);
    };
    auto displayedBuffer = [&] {
        const LRESULT row = SendMessageW(state.bufferCombo, CB_GETCURSEL, 0, 0);
        return SendMessageW(state.bufferCombo, CB_GETITEMDATA, row, 0);
    };
    auto chooseBuffer = [&](int value) {
        const LRESULT count = SendMessageW(state.bufferCombo, CB_GETCOUNT, 0, 0);
        for (LRESULT row = 0; row < count; ++row) {
            if (SendMessageW(state.bufferCombo, CB_GETITEMDATA, row, 0) != value) continue;
            SendMessageW(state.bufferCombo, CB_SETCURSEL, row, 0);
            SendMessageW(hwnd, WM_COMMAND, MAKEWPARAM(IDC_SETTINGS_BUFFER, CBN_SELCHANGE), 0);
            return;
        }
        Require(false, "requested buffer exists in fixture");
    };
    // Both legacy Shared milliseconds and IAudioClient3 frame periods must
    // survive the ASIO explanatory row, without a device query or rescan.
    for (bool sharedLowLatency : {false, true}) {
        state.probe.supported = sharedLowLatency;
        for (int milliseconds : kExclusiveBufferOptionsMs) {
            mode(1);
            chooseBuffer(milliseconds);
            mode(2);
            Require(!IsWindowEnabled(state.bufferCombo), "ASIO buffer is informational only");
            RememberCurrentBufferChoice(&state); // Also used when accepting ASIO.
            Require(state.selectedBufferMs == milliseconds && state.selectedSharedPeriodFrames == 960,
                "ASIO placeholder must not overwrite either saved WASAPI buffer");
            mode(1);
            Require(state.selectedBufferMs == milliseconds && displayedBuffer() == milliseconds &&
                    IsWindowEnabled(state.startButton),
                "Exclusive buffer and enabled Start survive an ASIO round trip");
            mode(0);
            const LRESULT sharedChoice = sharedLowLatency ? 960 : milliseconds;
            Require(displayedBuffer() == sharedChoice, "Shared uses its own buffer units");
            mode(2);
            mode(0);
            Require(displayedBuffer() == sharedChoice && state.selectedBufferMs == milliseconds &&
                    state.selectedSharedPeriodFrames == 960 && IsWindowEnabled(state.startButton),
                "Shared buffer survives ASIO with and without IAudioClient3");
        }
    }
    mode(1);
    chooseBuffer(30);
    state.exclusiveEndpointResults[0].recommendedBufferMs = 10;
    mode(2);
    mode(1);
    Require(displayedBuffer() == 30 && HasExclusiveVerificationForSelection(&state) &&
            IsWindowEnabled(state.startButton),
        "returning from ASIO retains a buffer above the verified Exclusive minimum");
    mode(2);
    Require(!state.exclusiveScanRunning && !state.sharedProbeCache && !state.captureAudioProbeCache,
        "buffer-only mode changes never start hardware probes");
    SendMessageW(state.preventSleepCheck, BM_SETCHECK, BST_UNCHECKED, 0);
    FinishSettingsDialog(hwnd, &state, true);
    Require(state.accepted && !IsWindow(hwnd) && g_settings.audioMode == AudioMode::Asio &&
            g_settings.wasapiBufferMs == 30 && g_settings.wasapiSharedPeriodFrames == 960,
        "accepting ASIO preserves both WASAPI preferences without saving a zero buffer");
    Require(!g_settings.preventSleep, "accepting settings applies the sleep prevention checkbox");
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    g_settings = saved;
    g_suppressSettingsSave = savedSuppress;
    g_exclusiveStartupFallback = savedFallback;
    std::puts("Settings buffers: all 6 Exclusive values, both Shared APIs, ASIO round trips and acceptance passed.");
}

static void TestSettingsSharedRefreshAcrossModes() {
    const auto saved = g_settings;
    const bool savedSuppress = g_suppressSettingsSave;
    g_suppressSettingsSave = true;
    WNDCLASSW wc{};
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpfnWndProc = SettingsControllerTestProc;
    wc.lpszClassName = L"LLCV_SETTINGS_SHARED_REFRESH_TEST";
    Require(RegisterClassW(&wc) != 0, "register Shared refresh fixture");
    struct Case { int mode; bool automatic; bool changeDefault; };
    for (const auto test : {Case{2, false, false}, Case{2, true, false},
                            Case{2, true, true}, Case{0, false, false}, Case{1, false, false}}) {
        g_settings = {};
        g_settings.audioOnly = true;
        g_settings.audioOutputDeviceId = L"A"; // The dialog may have an unaccepted choice of B.
        g_settings.asioDriverName = L"Refresh fixture ASIO";
        SettingsDialogState state;
        state.activeTab = SettingsTab::Audio;
        state.asioAvailable = true;
        state.asioDrivers = {{0, "Refresh fixture ASIO"}};
        state.audioEndpoints = {{L"A", L"Output A", true}, {L"B", L"Output B", false}};
        state.exclusiveEndpointResults.resize(2);
        for (auto& result : state.exclusiveEndpointResults) {
            result.state = ExclusiveEndpointState::Supported;
            result.recommendedBufferMs = 10;
        }
        state.exclusiveScanCompleted = 2;
        state.selectedBufferMs = 20;
        state.selectedSharedPeriodFrames = 960;
        HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"Hidden Shared refresh fixture", WS_POPUP,
            -32000, -32000, 1000, 650, nullptr, nullptr, wc.hInstance, &state);
        Require(hwnd != nullptr, "create Shared refresh fixture");
        const llcv::settings_ui::SettingsControlInitialValues initial{
            g_settings, false, true, kAppVersionLabel, VideoPreset::R1920x1080,
            {}, {}, kVideoPresets, kPcmQueueOptionsMs, {}};
        const llcv::settings_ui::SettingsControlPopulation population{
            &state, [](void* s) { PopulateAudioOutputCombo(static_cast<SettingsDialogState*>(s)); },
            [](void* s) { PopulateSettingsBufferCombo(static_cast<SettingsDialogState*>(s)); },
            [](void*) {}};
        llcv::settings_ui::CreateSettingsDialogControls(&state, hwnd, wc.hInstance, initial, population);
        std::array<std::atomic<unsigned>, 2> queries{};
        std::atomic<unsigned> minimum{48}, inventoryQueries{0};
        state.sharedProbeCache = std::make_unique<llcv::settings_ui::SettingsModeCache<AudioClient3Support>>(
            [&](const auto& key) {
                Require(key.device == L"A" || key.device == L"B", "Shared query uses endpoint identity");
                ++queries[key.device == L"B" ? 1 : 0];
                return AudioClient3Support{true, S_OK, 480, 48, minimum.load(), 1920, 0.0};
            }, [hwnd] { PostMessageW(hwnd, WM_AUDIOCLIENT3_PROBE_COMPLETE, 0, 0); });
        state.inventoryCache = std::make_unique<llcv::settings_ui::SettingsModeCache<SettingsDeviceInventory>>(
            [&, test](const auto&) {
                ++inventoryQueries;
                SettingsDeviceInventory inventory;
                // Reordering must not turn a saved endpoint identity into a list index.
                inventory.audioOutputs = {{L"B", L"Output B", test.changeDefault},
                                         {L"A", L"Output A", !test.changeDefault}};
                return inventory;
            }, [hwnd] { PostMessageW(hwnd, WM_SETTINGS_INVENTORY_READY, 0, 0); });
        state.videoModeCache = std::make_unique<llcv::settings_ui::SettingsModeCache<SettingsVideoModes>>(
            [](const auto&) { return SettingsVideoModes{}; },
            [hwnd] { PostMessageW(hwnd, WM_SETTINGS_VIDEO_MODES_READY, 0, 0); });
        state.captureAudioProbeCache = std::make_unique<llcv::settings_ui::SettingsModeCache<InternalCaptureAudioProbe>>(
            [](const auto&) { return InternalCaptureAudioProbe{}; },
            [hwnd] { PostMessageW(hwnd, WM_CAPTURE_AUDIO_PROBE_COMPLETE, 0, 0); });
        const auto pump = [&](auto done) {
            const auto deadline = GetTickCount64() + 3000;
            while (!done() && GetTickCount64() < deadline) {
                MsgWaitForMultipleObjects(0, nullptr, FALSE, 10, QS_ALLINPUT);
                MSG message{};
                while (PeekMessageW(&message, hwnd, 0, 0, PM_REMOVE)) DispatchMessageW(&message);
            }
            Require(done(), "injected refresh work completes without hardware");
        };
        const auto output = [&](int row) {
            SendMessageW(state.audioOutputCombo, CB_SETCURSEL, row, 0);
            SendMessageW(hwnd, WM_COMMAND, MAKEWPARAM(IDC_SETTINGS_AUDIO_OUTPUT, CBN_SELCHANGE), 0);
            pump([&] { return state.probeReady; });
        };
        const auto mode = [&](int selection) {
            SendMessageW(state.audioCombo, CB_SETCURSEL, selection, 0);
            SendMessageW(hwnd, WM_COMMAND, MAKEWPARAM(IDC_SETTINGS_AUDIO, CBN_SELCHANGE), 0);
        };
        output(1); output(2); // Cache both endpoints before refreshing only the selected one.
        output(test.automatic ? 0 : 2);
        mode(test.mode);
        minimum = 96;
        SendMessageW(hwnd, WM_COMMAND, MAKEWPARAM(IDC_SETTINGS_VIDEO_REFRESH, BN_CLICKED), 0);
        pump([&] { return !state.inventoryPending; });
        if (test.mode != 0) Require(queries[0] == 1 && queries[1] == 1,
            "refresh outside Shared only invalidates; it does not eagerly probe audio");
        mode(0);
        pump([&] { return state.probeReady; });
        const size_t target = !test.automatic || test.changeDefault ? 1 : 0;
        Require(state.probe.minimumFrames == 96 && state.appliedSharedEndpoint == (target ? L"B" : L"A"),
            "Shared return uses refreshed capabilities of the preserved selection/current default");
        Require(!state.sharedPeriodChoices.empty() && state.sharedPeriodChoices.front() == 96 &&
                state.selectedSharedPeriodFrames == 960 && state.selectedBufferMs == 20,
            "refreshed buffer choices preserve the user's still-valid buffer preferences");
        Require(SelectedAudioEndpointId(&state) == (test.automatic ? L"" : L"B") &&
                g_settings.audioOutputDeviceId == L"A",
            "refresh preserves unaccepted selection and never changes saved preferences");
        for (unsigned repeat = 0; repeat < 20; ++repeat) { mode(2); mode(1); mode(0); }
        Require(queries[target] == 2 && queries[1-target] == 1 && inventoryQueries == 1,
            "one refresh queries only its target once; subsequent mode changes remain cached");
        Require(!state.exclusiveScanRunning && !state.exclusiveProbeThread.joinable() &&
                state.exclusiveScanCompleted == 2 && IsWindowEnabled(state.startButton),
            "Shared refresh preserves Exclusive verdicts and start eligibility without rescanning");
        state.sharedProbeCache.reset(); state.inventoryCache.reset();
        state.videoModeCache.reset(); state.captureAudioProbeCache.reset();
        DestroyWindow(hwnd);
    }
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    g_settings = saved;
    g_suppressSettingsSave = savedSuppress;
    std::puts("Shared refresh: ASIO explicit/default/default-change, Shared/Exclusive controls, preserved buffers and bounded query counts passed.");
}

static void TestSettingsAudioIdentityAndRefresh() {
    const auto saved = g_settings;
    g_settings = {};
    g_settings.audioOutputDeviceId = L"B";
    g_settings.asioDriverName = L"Driver one";
    WNDCLASSW wc{};
    wc.hInstance=GetModuleHandleW(nullptr); wc.lpfnWndProc=SettingsControllerTestProc;
    wc.lpszClassName=L"LLCV_SETTINGS_IDENTITY_TEST";
    Require(RegisterClassW(&wc)!=0,"register identity fixture");
    SettingsDialogState state;
    state.activeTab=SettingsTab::Audio;
    state.asioAvailable=true;
    state.asioDrivers={{0,"Driver one"},{1,"Driver two"}};
    for (const auto* id : {L"A",L"B",L"C"}) {
        AudioEndpointInfo endpoint{}; endpoint.id=id; endpoint.name=id; endpoint.isDefault=id==std::wstring(L"A");
        state.audioEndpoints.push_back(endpoint);
    }
    state.exclusiveEndpointResults.resize(3);
    for (auto& result:state.exclusiveEndpointResults) result.state=ExclusiveEndpointState::Supported;
    state.exclusiveScanCompleted=3;
    HWND hwnd=CreateWindowExW(0,wc.lpszClassName,L"Hidden identity fixture",WS_POPUP,
        -32000,-32000,1000,650,nullptr,nullptr,wc.hInstance,&state);
    Require(hwnd!=nullptr,"create identity fixture");
    const llcv::settings_ui::SettingsControlInitialValues initial{
        g_settings,false,true,kAppVersionLabel,VideoPreset::R1920x1080,
        {},{},kVideoPresets,kPcmQueueOptionsMs,{}};
    const llcv::settings_ui::SettingsControlPopulation population{
        &state,[](void* s){PopulateAudioOutputCombo(static_cast<SettingsDialogState*>(s));},
        [](void* s){PopulateSettingsBufferCombo(static_cast<SettingsDialogState*>(s));},[](void*){}};
    llcv::settings_ui::CreateSettingsDialogControls(&state,hwnd,wc.hInstance,initial,population);
    std::atomic<unsigned> queries{0};
    HANDLE started=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    HANDLE release=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    state.sharedProbeCache=std::make_unique<llcv::settings_ui::SettingsModeCache<AudioClient3Support>>(
        [&](const auto& key) {
            ++queries;
            if (key.device==L"B") {
                SetEvent(started);
                Require(WaitForSingleObject(release,3000)==WAIT_OBJECT_0,"release slow fake shared driver");
            }
            AudioClient3Support result{}; result.minimumFrames=key.device==L"B"?48:96;
            return result;
        },[hwnd]{PostMessageW(hwnd,WM_AUDIOCLIENT3_PROBE_COMPLETE,0,0);});
    auto mode=[&](int value) {
        SendMessageW(state.audioCombo,CB_SETCURSEL,value,0);
        SendMessageW(hwnd,WM_COMMAND,MAKEWPARAM(IDC_SETTINGS_AUDIO,CBN_SELCHANGE),0);
    };
    RequestSettingsSharedProbe(&state,hwnd);
    Require(WaitForSingleObject(started,2000)==WAIT_OBJECT_0,"fake query starts off UI thread");
    const auto before=GetTickCount64();
    SendMessageW(state.audioOutputCombo,CB_SETCURSEL,3,0);
    SendMessageW(hwnd,WM_COMMAND,MAKEWPARAM(IDC_SETTINGS_AUDIO_OUTPUT,CBN_SELCHANGE),0);
    mode(2);
    Require(SelectedAsioDriverName(&state)==L"Driver one","WASAPI index never selects unrelated ASIO driver");
    SendMessageW(state.audioOutputCombo,CB_SETCURSEL,1,0);
    mode(0);
    Require(SelectedAudioEndpointId(&state)==L"C","return from ASIO preserves actual WASAPI endpoint ID");
    Require(GetTickCount64()-before<500,"mode/output switches do not join slow driver query");
    SetEvent(release);
    const auto deadline=GetTickCount64()+3000;
    while ((!state.probeReady || state.appliedSharedEndpoint!=L"C") && GetTickCount64()<deadline) {
        MsgWaitForMultipleObjects(0,nullptr,FALSE,20,QS_ALLINPUT);
        MSG message{};
        while(PeekMessageW(&message,hwnd,0,0,PM_REMOVE)) DispatchMessageW(&message);
    }
    Require(state.probeReady && state.probe.minimumFrames==96 && queries==2,
        "late result B cannot overwrite selected C and pending requests coalesce");
    for(int i=0;i<30;++i) {
        mode(2); Require(SelectedAsioDriverName(&state)==L"Driver two","ASIO selection retained independently");
        mode(1); Require(SelectedAudioEndpointId(&state)==L"C","Exclusive shares WASAPI identity");
        mode(0);
    }
    Require(queries==2 && !state.exclusiveScanRunning,"cached mode changes never repeat probes");

    state.captureDevices={{L"capture-B",L"Card B"},{L"capture-A",L"Card A"}};
    state.captureAudioDevices={{L"mic-B",L"Mic B"}};
    llcv::settings_ui::PopulateCaptureDevicePicker(state.captureDeviceCombo,state.captureDevices,L"capture-B",false,false);
    llcv::settings_ui::PopulateCaptureDevicePicker(state.captureAudioDeviceCombo,state.captureAudioDevices,L"mic-B",true,false);
    SettingsDeviceInventory changed;
    changed.videoInputs={{L"capture-A",L"Card A"},{L"capture-B",L"Card B"},{L"capture-new",L"New card"}};
    changed.audioInputs={{L"mic-new",L"New mic"},{L"mic-B",L"Mic B"}};
    changed.audioOutputs={state.audioEndpoints[2],state.audioEndpoints[0],state.audioEndpoints[1]};
    state.exclusiveEndpointResults[2].recommendedBufferMs=17;
    ApplySettingsInventory(&state,changed);
    Require(SelectedCaptureDeviceId(&state)==L"capture-B" && SelectedCaptureAudioDeviceId(&state)==L"mic-B" &&
        SelectedAudioEndpointId(&state)==L"C","inventory reorder preserves all selected IDs");
    Require(state.captureDevices.size()==3 && state.captureAudioDevices.size()==2 &&
        state.exclusiveEndpointResults[0].recommendedBufferMs==17 && state.exclusiveScanCompleted==3,
        "refresh discovers attached devices and retains Exclusive verdicts by endpoint identity");
    changed.videoInputs={{L"capture-A",L"Card A"}};
    changed.audioOutputs={state.audioEndpoints[1]};
    ApplySettingsInventory(&state,changed);
    Require(SelectedCaptureDeviceId(&state)==L"capture-B" && SelectedAudioEndpointId(&state)==L"C",
        "disconnection never silently replaces explicit selections with default");
    state.sharedProbeCache.reset();
    CloseHandle(started); CloseHandle(release);
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName,wc.hInstance);
    g_settings=saved;
    std::puts("Settings identity: 3 endpoints, 2 ASIO drivers, slow/stale probe, cache reuse and inventory reorder passed.");
}
