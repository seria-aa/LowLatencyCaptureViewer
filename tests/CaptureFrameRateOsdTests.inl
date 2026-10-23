// Included by VsrExperimentTests. Exercises production diagnostic text and
// private fonts with CPU-only DirectWrite; no window, GPU, capture or audio I/O.
static void TestCaptureFrameRateOsd() {
    const auto savedSettings = g_settings;
    const auto savedCaptureName = g_activeCaptureDeviceName;
    const auto savedOutputName = ActiveAudioOutputName();
    const bool savedLatched = g_resolutionPlanLatched;
    const bool savedSplit = g_splitResolutionPlan;
    const bool savedWarning = g_captureFrameRateOverdelivery.load();
    const int savedConfigured = g_videoConfiguredFps.load();
    const double savedInputFps = g_osdInputFps.load();
    const double savedPresentFps = g_osdPresentFps.load();
    const bool savedExclusive = g_videoExclusiveActive.load();
    const bool savedHdr = g_hdrOutputActive.load();
    const int savedHdrDisplay = g_hdrDisplayState.load();

    g_settings = {};
    g_settings.shortcuts = keys::Defaults;
    g_settings.videoPreset = g_settings.capturePreset = VideoPreset::R1920x1080;
    g_settings.relativeWindowSize = false;
    g_settings.pixelPerfect = true;
    g_resolutionPlanLatched = false;
    g_splitResolutionPlan = false;
    g_activeCaptureDeviceName = L"GC573";
    SetActiveAudioOutputName(L"Speakers");

    Microsoft::WRL::ComPtr<IDWriteFactory> factory;
    Check(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
        reinterpret_cast<IUnknown**>(factory.GetAddressOf())), "FPS OSD DirectWrite factory");
    llcv::overlay_ui::Fonts fonts;
    Check(fonts.Initialize(factory.Get()), "FPS OSD production font resources");
    Require(fonts.Available(), "FPS OSD measures the bundled font, not a system substitution");
    unsigned measuredLayouts = 0;
    float maximumHeight = 0.0f;
    {
    DirectD3D11Renderer renderer;
#ifdef LLCV_VSR_FEATURE
    renderer.vsrInputWidth = 1920;
    renderer.vsrInputHeight = 1080;
    renderer.vsrDisplayWidth = 3840;
    renderer.vsrDisplayHeight = 2160;
#endif

    struct RateCase { int setting; bool capture40; bool audioOnly; int request; };
    const RateCase cases[] = {
        {0, false, false, 120},      // Auto never carries a stale manual warning.
        {30, false, false, 30},
        {40, false, false, 40},
        {60, false, false, 60},
        {120, false, false, 120},
        {240, false, false, 240},
        {120, true, false, 40},     // Separate 40 FPS checkbox overrides the list.
        {60, false, true, 60}       // Audio-only mode never displays capture warnings.
    };
    for (UiLanguage language : {UiLanguage::Korean, UiLanguage::English}) {
        g_settings.uiLanguage = language;
        const bool english = IsEnglishUi();
        Microsoft::WRL::ComPtr<IDWriteTextFormat> format;
        Check(fonts.CreateFormat(factory.Get(), DWRITE_FONT_WEIGHT_MEDIUM, 15.0f,
                                 english, &format), "FPS OSD production text format");
        Check(format->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, 20.0f, 15.0f),
              "FPS OSD production line spacing");
        for (const auto& rate : cases) {
            g_settings.videoFrameRate = rate.setting;
            g_settings.capture40Fps = rate.capture40;
            g_settings.audioOnly = rate.audioOnly;
            g_videoConfiguredFps = rate.request;
            g_osdInputFps = rate.request * 2.0;
            g_osdPresentFps = rate.request * 2.0;
            const int requested = RequestedVideoFrameRate();
            if (requested != rate.request)
                std::fprintf(stderr, "FPS OSD fixture: setting=%d override40=%d actual=%d expected=%d\n",
                    rate.setting, rate.capture40, requested, rate.request);
            Require(requested == rate.request,
                    "FPS OSD fixture uses the production requested rate");
            g_captureFrameRateOverdelivery = false;
            const std::wstring baseline = BuildRuntimeOsdText(3840, 2160);
            Require(std::count(baseline.begin(), baseline.end(), L'\n') == 18,
                    "baseline diagnostics retain their nineteen logical lines");
            for (bool warning : {false, true}) {
                g_captureFrameRateOverdelivery = warning;
                const bool expectedWarning = warning && !rate.audioOnly &&
                                             (rate.setting > 0 || rate.capture40);
                std::wstring text = BuildRuntimeOsdText(3840, 2160);
                const std::wstring suffix = english
                    ? L" · Above requested " + std::to_wstring(rate.request)
                    : L" · 요청 " + std::to_wstring(rate.request) + L" 초과";
                const auto suffixPosition = text.find(suffix);
                Require((suffixPosition != std::wstring::npos) == expectedWarning,
                        "only active manual capture warnings name the requested FPS");
                const auto fpsPosition = text.find(english ? L"Actual FPS" : L"실제 FPS");
                Require(fpsPosition != std::wstring::npos, "actual FPS diagnostics remain present");
                const auto fpsEnd = text.find(L'\n', fpsPosition);
                if (expectedWarning) {
                    Require(suffixPosition > fpsPosition && suffixPosition < fpsEnd &&
                            text.find(suffix, suffixPosition + suffix.size()) == std::wstring::npos,
                            "FPS warning occurs once on the actual FPS line");
                }
                std::wstring withoutWarning = text;
                if (expectedWarning) withoutWarning.erase(suffixPosition, suffix.size());
                Require(withoutWarning == baseline,
                        "warning leaves all existing capture and audio diagnostics unchanged");
                Require(std::count(text.begin(), text.end(), L'\n') == 18,
                        "warning never adds a bottom row to the fixed diagnostics panel");

#ifdef LLCV_VSR_FEATURE
                for (auto state : {llcv::vsr::State::Off, llcv::vsr::State::Requested,
                        llcv::vsr::State::Bypassed, llcv::vsr::State::Rejected,
                        llcv::vsr::State::Unknown}) {
                    renderer.vsrState = state;
                    std::wstring rendered = text;
                    // Production refreshOverlayLayouts places VSR after Display.
                    size_t videoEnd = 0;
                    for (int line = 0; line < 5; ++line) {
                        const auto end = rendered.find(L'\n', videoEnd);
                        Require(end != std::wstring::npos, "VSR insertion line exists");
                        videoEnd = end + 1;
                    }
                    rendered.insert(videoEnd, renderer.vsrOsdLine());
#else
                {
                    const std::wstring& rendered = text;
#endif
                    Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
                    Check(factory->CreateTextLayout(rendered.c_str(),
                        static_cast<UINT32>(rendered.size()), format.Get(),
                        DirectD3D11Renderer::kOsdTextWidth,
                        DirectD3D11Renderer::kOsdTextHeight, &layout), "FPS OSD text layout");
                    const auto headingEnd = rendered.find(L'\n');
                    Check(layout->SetFontWeight(DWRITE_FONT_WEIGHT_SEMI_BOLD,
                        DWRITE_TEXT_RANGE{0, static_cast<UINT32>(headingEnd)}),
                        "FPS OSD production heading weight");
                    DWRITE_TEXT_METRICS metrics{};
                    Check(layout->GetMetrics(&metrics), "FPS OSD text metrics");
                    Require(metrics.height <= DirectD3D11Renderer::kOsdTextHeight &&
                            metrics.widthIncludingTrailingWhitespace <= DirectD3D11Renderer::kOsdTextWidth,
                            "bilingual diagnostics and all VSR states fit the unchanged panel");
                    Require(metrics.lineCount ==
                            std::count(rendered.begin(), rendered.end(), L'\n') + 1,
                            "no diagnostic line wraps or pushes another line below the panel");
                    if (expectedWarning) {
                        const auto position = rendered.find(suffix);
                        FLOAT x = 0.0f, y = 0.0f;
                        DWRITE_HIT_TEST_METRICS hit{};
                        Check(layout->HitTestTextPosition(static_cast<UINT32>(
                            position + suffix.size() - 1), FALSE, &x, &y, &hit),
                            "FPS warning final glyph position");
                        Require(hit.top >= 0.0f && hit.left >= 0.0f &&
                                hit.top + hit.height <= DirectD3D11Renderer::kOsdTextHeight &&
                                hit.left + hit.width <= DirectD3D11Renderer::kOsdTextWidth,
                                "the complete FPS warning remains inside the actual clip rectangle");
                    }
                    maximumHeight = (std::max)(maximumHeight, metrics.height);
                    ++measuredLayouts;
                }
            }
        }
    }
    } // Destroy the uninitialized renderer before restoring its diagnostic globals.

    g_settings = savedSettings;
    g_activeCaptureDeviceName = savedCaptureName;
    SetActiveAudioOutputName(savedOutputName);
    g_resolutionPlanLatched = savedLatched;
    g_splitResolutionPlan = savedSplit;
    g_captureFrameRateOverdelivery = savedWarning;
    g_videoConfiguredFps = savedConfigured;
    g_osdInputFps = savedInputFps;
    g_osdPresentFps = savedPresentFps;
    g_videoExclusiveActive = savedExclusive;
    g_hdrOutputActive = savedHdr;
    g_hdrDisplayState = savedHdrDisplay;
    std::printf("Capture FPS OSD: %u CPU-only bilingual layouts; max height %.1f / %.1f.\n",
                measuredLayouts, maximumHeight, DirectD3D11Renderer::kOsdTextHeight);
}
