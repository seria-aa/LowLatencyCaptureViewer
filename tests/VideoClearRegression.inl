// Synthetic GPU comparison only: no capture/audio devices, Present calls,
// visible windows, exclusive mode or desktop/display setting changes.
// Include after VsrExperimentTests.cpp's OutputPixels/Check/Require helpers.
static int RunVideoClearRegressionTests() {
    const auto savedSettings = g_settings;
    const bool savedFullscreen = g_fullscreen.load();
    const bool savedSourceSize = g_fullscreenSourceSize.exchange(false);
    const bool savedOsd = g_osdVisible.load();
    const bool savedAudioOsd = g_audioOsdVisible.load();
    const auto savedHudUntil = g_volumeHudUntilMs.load();
    const auto savedHudContent = g_transientHudContent.load();
    const auto savedVsrMode = g_vsrMode.load();
    const auto savedOutputGeneration = g_outputConfigurationGeneration.load();
    const auto savedOverlayGeneration = g_overlayGeneration.load();
#ifdef LLCV_HDR_SCRGB_PROTOTYPE
    const bool savedScrgb = g_useScrgbPrototype;
    g_useScrgbPrototype = false;
#endif
    g_settings.pixelPerfect = false;
    g_settings.audioOnly = false;
    g_settings.presentationMode = PresentationMode::AllowTearing;
    g_settings.scalingMode = ScalingMode::Smooth;
    g_settings.videoPreset = VideoPreset::R1280x720;
    // This fixture checks a fixed 720p display size, independently of the
    // default capture-matched mode covered by the display-sizing regressions.
    g_settings.displayMatchesCapture = false;
    g_fullscreen = false;
    g_osdVisible = false;
    g_audioOsdVisible = false;
    g_volumeHudUntilMs = 0;
    g_vsrMode = llcv::vsr::Mode::Disabled;
    HWND window = CreateWindowExW(0, L"STATIC", L"Hidden border-clear regression",
        WS_POPUP, 0, 0, 640, 360, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    Require(window != nullptr, "clear regression hidden window");
    auto restore = [&] {
        DestroyWindow(window);
        g_settings = savedSettings;
        g_fullscreen = savedFullscreen;
        g_fullscreenSourceSize = savedSourceSize;
        g_osdVisible = savedOsd;
        g_audioOsdVisible = savedAudioOsd;
        g_volumeHudUntilMs = savedHudUntil;
        g_transientHudContent = savedHudContent;
        g_vsrMode = savedVsrMode;
        g_outputConfigurationGeneration = savedOutputGeneration;
        g_overlayGeneration = savedOverlayGeneration;
#ifdef LLCV_HDR_SCRGB_PROTOTYPE
        g_useScrgbPrototype = savedScrgb;
#endif
    };
    constexpr int inputWidth = 128, inputHeight = 72;
    unsigned comparisons = 0, borders = 0, noBorders = 0, sharpCases = 0;
    unsigned vsrOnAccepted = 0, vsrOffAccepted = 0, vsrRejected = 0;
    unsigned vsrUnavailable = 0, vsrBypassed = 0;
    for (const auto format : {VideoPixelFormat::Nv12, VideoPixelFormat::Yuy2}) {
        Require(SetWindowPos(window, nullptr, 0, 0, 640, 360,
            SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE), "clear regression initial geometry");
        DirectD3D11Renderer renderer;
        const HRESULT initialized = renderer.initialize(window, inputWidth, inputHeight, 60, format);
        if (initialized == DXGI_ERROR_UNSUPPORTED || initialized == DXGI_ERROR_SDK_COMPONENT_MISSING) {
            std::puts("SKIP: clear regression GPU format/debug layer unavailable");
            restore();
            return 77;
        }
        Check(initialized, "clear regression SDR renderer");
        renderer.fullscreenDisplaySize = true;
        g_fullscreenSourceSize = true;
        Check(renderer.configureVideoOutput(inputWidth, inputHeight), "F5 source-size output");
        BOOL f5Enabled = FALSE; RECT f5Rect{};
        renderer.videoContext->VideoProcessorGetStreamDestRect(renderer.processor, 0, &f5Enabled, &f5Rect);
        Require(f5Enabled && f5Rect.right-f5Rect.left == inputWidth &&
                f5Rect.bottom-f5Rect.top == inputHeight,
                "F5 source-size override reaches actual VP destination");
        g_fullscreenSourceSize = false;
        Check(renderer.configureVideoOutput(inputWidth, inputHeight), "restore selected fullscreen size");
        renderer.videoContext->VideoProcessorGetStreamDestRect(renderer.processor, 0, &f5Enabled, &f5Rect);
        Require(f5Enabled && f5Rect.right-f5Rect.left == 640 &&
                f5Rect.bottom-f5Rect.top == 360,
                "clearing F5 override restores selected display resolution");
        renderer.fullscreenDisplaySize = false;
        const UINT stride = inputWidth * (format == VideoPixelFormat::Yuy2 ? 2u : 1u);
        std::vector<BYTE> input(static_cast<size_t>(stride) *
            (format == VideoPixelFormat::Yuy2 ? inputHeight : inputHeight * 3 / 2), 128);
        for (int y = 0; y < inputHeight; ++y) {
            for (int x = 0; x < inputWidth; ++x) {
                const BYTE luma = static_cast<BYTE>(32 + (x * 7 + y * 11) % 190);
                input[static_cast<size_t>(y) * stride +
                    x * (format == VideoPixelFormat::Yuy2 ? 2u : 1u)] = luma;
            }
        }
        using Position = llcv::settings::FullscreenVideoPosition;
        struct Geometry { LONG width, height; bool fullscreen; Position position = Position::Center; };
        for (const auto scaling : {ScalingMode::Smooth, ScalingMode::Sharp}) {
          g_settings.scalingMode = scaling;
          for (const Geometry geometry : {Geometry{640, 360, false}, {640, 480, false},
                   {640, 480, true}, {96, 54, false}, {320, 240, false}, {640, 360, false},
                   {1600, 900, true, Position::TopLeft}, {1600, 900, true, Position::Center},
                   {1600, 900, true, Position::BottomRight}}) {
           for (const auto requestedVsr : {llcv::vsr::Mode::Disabled,
                    llcv::vsr::Mode::Off, llcv::vsr::Mode::On}) {
            // Every setting starts with a fresh processor. Disabled deliberately
            // leaves a new processor untouched; it is not an ON->OFF operation.
            g_vsrMode = llcv::vsr::Mode::Disabled;
            Require(SetWindowPos(window, nullptr, 0, 0, geometry.width, geometry.height,
                SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE), "clear regression resize geometry");
            ++g_outputConfigurationGeneration;
            Check(renderer.resizeOutput(window), "clear regression output-only resize");
            D3D11_VIDEO_COLOR blackBackground{};
            blackBackground.RGBA.A = 1.0f;
            renderer.videoContext->VideoProcessorSetOutputBackgroundColor(
                renderer.processor, FALSE, &blackBackground);
            // Exercise fullscreen placement without entering actual exclusive mode.
            renderer.fullscreenDisplaySize = geometry.fullscreen;
            g_settings.fullscreenVideoPosition = geometry.position;
            Check(renderer.configureVideoOutput(inputWidth, inputHeight), "clear regression VP geometry");
            const RECT expected = llcv::video_placement::Destination(inputWidth, inputHeight,
                geometry.width, geometry.height, geometry.fullscreen, 1280, 720, geometry.position);
            BOOL enabled = FALSE; RECT destination{};
            renderer.videoContext->VideoProcessorGetStreamDestRect(renderer.processor, 0, &enabled, &destination);
            Require(enabled && EqualRect(&expected, &destination), "actual VP uses selected display size/position");
            Require(renderer.skipRedundantBorderClear,
                "known opaque SDR full VP target enables redundant clear omission after configure/resize");
            if (requestedVsr != llcv::vsr::Mode::Disabled) {
                if (!renderer.vsrEligible) {
                    ++vsrBypassed;
                    continue;
                }
                g_vsrMode = requestedVsr;
                const HRESULT applied = renderer.applyVsrMode();
                if (FAILED(applied)) {
                    // Unsupported extension with unknown OFF state: do not use
                    // it. The next iteration recreates the processor before any
                    // further comparisons, just as device recovery would.
                    ++vsrUnavailable;
                    continue;
                }
            }
            Require(renderer.skipRedundantBorderClear,
                "hot VSR mode change preserves known full-target clear policy");
            if (renderer.sharpScalingActive) ++sharpCases;
            if (renderer.pixelPerfectBorders) ++borders; else ++noBorders;
            auto render = [&](bool skip, bool osd, bool sentinel) {
                renderer.context->OMSetRenderTargets(0, nullptr, nullptr);
                if (sentinel) {
                    // Nonblack RGB and nonopaque alpha expose unwritten old
                    // borders that fresh zero-initialized textures can conceal.
                    const float magenta[4]{1.0f, 0.0f, 1.0f, 0.25f};
                    renderer.context->ClearRenderTargetView(renderer.backBufferRenderTarget, magenta);
                }
                renderer.skipRedundantBorderClear = skip;
                renderer.upload(input.data(), stride);
                D3D11_VIDEO_PROCESSOR_STREAM stream{};
                stream.Enable = TRUE;
                stream.pInputSurface = renderer.inputViews[renderer.activeUploadSurface];
                // Same preparation as presentUploaded, stopping before Present
                // so hidden-window occlusion and flip do not invalidate readback.
                if (renderer.pixelPerfectBorders && !renderer.skipRedundantBorderClear) {
                    const float black[4]{0.0f, 0.0f, 0.0f, 1.0f};
                    renderer.context->ClearRenderTargetView(renderer.backBufferRenderTarget, black);
                }
                Check(renderer.processVideo(stream), "clear regression VP frame");
                // VSR state changes can publish a transient HUD. This regression
                // compares the diagnostic OSD explicitly, with timed HUDs hidden.
                g_volumeHudUntilMs = 0;
                g_osdVisible = osd;
                Check(renderer.drawOverlayQuads(), "clear regression OSD draw");
                renderer.context->OMSetRenderTargets(0, nullptr, nullptr);
                return OutputPixels(renderer);
            };
            // Let a newly requested VSR path settle on the identical synthetic
            // input before comparing it against itself with/without the clear.
            // No Present, timing metric, or claim of visual VSR activation.
            for (int frame = 0; frame < 8; ++frame) {
                renderer.upload(input.data(), stride);
                D3D11_VIDEO_PROCESSOR_STREAM stream{};
                stream.Enable = TRUE;
                stream.pInputSurface = renderer.inputViews[renderer.activeUploadSurface];
                Check(renderer.processVideo(stream), "clear regression VSR preparation");
            }
            if (requestedVsr != llcv::vsr::Mode::Disabled) {
                if (renderer.vsrState == llcv::vsr::State::Requested) ++vsrOnAccepted;
                else if (renderer.vsrState == llcv::vsr::State::Off) ++vsrOffAccepted;
                else if (renderer.vsrState == llcv::vsr::State::Rejected) ++vsrRejected;
                else Require(false, "eligible VSR test remains in a known safe state");
            }
            const auto reference = render(false, false, true);
            Require(reference == render(true, false, true),
                "full BGRA output exactly matches original clear including alpha and all borders");
            ++comparisons;
            ++g_overlayGeneration;
            const auto referenceOsd = render(false, true, true);
            const auto optimizedOsd = render(true, true, true);
            Require(referenceOsd == optimizedOsd,
                "OSD BGRA pixels exactly match original clear on video and borders");
            Require(referenceOsd != reference, "OSD actually drew pixels in clear regression");
            ++comparisons;
            Require(reference == render(true, false, false),
                "next VP frame removes old visible OSD from video and black borders without clear");
            ++comparisons;
            // Process another frame on the same buffer to detect stale edge
            // data hidden by first-use initialization or upload-surface rotation.
            Require(reference == render(true, false, false),
                "repeated no-clear frame preserves entire BGRA output exactly");
            ++comparisons;
           }
          }
        }
        renderer.reset();
        Require(!renderer.skipRedundantBorderClear, "renderer reset invalidates cached clear omission");
    }
    Require(borders != 0 && noBorders != 0, "clear regression covers border and full video geometries");
    {
        g_vsrMode = llcv::vsr::Mode::Disabled;
        DirectD3D11Renderer hdr;
        const HRESULT initialized = hdr.initialize(window, inputWidth, inputHeight, 60,
            VideoPixelFormat::P010, true);
        if (initialized == DXGI_ERROR_UNSUPPORTED || initialized == DXGI_ERROR_SDK_COMPONENT_MISSING) {
            std::puts("SKIP: HDR GPU/output unavailable; conservative HDR policy remains unit-tested");
        } else {
            Check(initialized, "clear regression HDR renderer");
            Require(hdr.hdrOutput && !hdr.skipRedundantBorderClear,
                "HDR retains original border-clear behavior");
            Require(SetWindowPos(window, nullptr, 0, 0, 640, 480,
                SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE), "clear regression HDR resize geometry");
            ++g_outputConfigurationGeneration;
            Check(hdr.resizeOutput(window), "clear regression HDR resize");
            Require(hdr.hdrOutput && !hdr.skipRedundantBorderClear,
                "HDR resize does not reuse SDR clear omission");
            for (int i = 0; i < llcv::settings::kFullscreenVideoPositionCount; ++i) {
                const auto position = llcv::settings::FullscreenPositionFromIndex(i);
                g_settings.fullscreenVideoPosition = position;
                hdr.fullscreenDisplaySize = true;
                Check(hdr.configureVideoOutput(inputWidth, inputHeight), "HDR fullscreen position");
                const RECT expected = llcv::video_placement::Destination(inputWidth, inputHeight,
                    640, 480, true, 1280, 720, position);
                BOOL enabled = FALSE; RECT destination{};
                hdr.videoContext->VideoProcessorGetStreamDestRect(hdr.processor, 0, &enabled, &destination);
                Require(enabled && EqualRect(&expected, &destination) && !hdr.skipRedundantBorderClear,
                    "HDR position and conservative clear preserved");
            }
        }
    }
    restore();
    std::printf("PASS: %u exact full-frame BGRA comparisons; %u bordered/%u full-video cases, "
        "NV12/YUY2, Smooth/Sharp, resize, stale sentinel and OSD show/hide; HDR retains clear.\n",
        comparisons, borders, noBorders);
    std::printf("Clear regression capability counts: Sharp active=%u; VSR ON request accepted=%u, "
        "OFF request accepted=%u, rejected with safe OFF=%u, unavailable/unknown skipped=%u, "
        "ineligible skipped=%u. Request acceptance is not proof of VSR effect activation.\n",
        sharpCases, vsrOnAccepted, vsrOffAccepted, vsrRejected, vsrUnavailable, vsrBypassed);
    return 0;
}
