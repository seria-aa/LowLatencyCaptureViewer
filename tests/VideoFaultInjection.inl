// Regression tests for injected driver faults. Also runnable with --faults.
// This file is test-only and calls the production format functions directly.

static int RunVideoFaultInjection() {
    using namespace llcv::video;
    const int before = failures;
    FakeVideoPin pin;
    DWORD bytes = 0;
    UINT32 stride = 0;
    int fps = 0;
    VideoPixelFormat format = VideoPixelFormat::Auto;

    pin.modes = {{120}, {60}};
    pin.modes[0].imageBytes = 1;
    auto hr = ConfigureVideoPin(&pin, 1920, 1080, 120, VideoPixelFormat::Nv12,
                                bytes, stride, fps, format, nullptr);
    std::printf("PROBE invalid-preferred: hr=%08X fallback=%d attempts=%zu\n",
        unsigned(hr), fps, pin.attemptedRates.size());
    Check(SUCCEEDED(hr) && fps == 60,
          "fault: invalid preferred payload must not prevent a valid lower-rate fallback");
    Check(pin.attemptedRates == std::vector<int>{60},
          "invalid preferred payload is rejected before touching the device configuration");

    pin.modes = {{60}};
    pin.modes[0].fps = 0;
    pin.modes[0].minimumInterval = 41667;
    pin.modes[0].maximumInterval = 166667;
    HRESULT query = S_OK;
    auto modes = ProbePixelFormats(&pin, 1920, 1080, &query);
    std::printf("PROBE omitted-default-timing: hr=%08X modes=%zu\n", unsigned(query), modes.size());
    Check(HasRate(modes, 120),
          "fault: explicit valid frame interval range should remain usable when default timing is absent");
    pin.attemptedRates.clear();
    hr = ConfigureVideoPin(&pin, 1920, 1080, 120, VideoPixelFormat::Nv12,
                           bytes, stride, fps, format, nullptr);
    Check(SUCCEEDED(hr) && fps == 120,
          "fault: requested interval-supported mode should be attempted without default timing");
    pin.rejectedRate = 120;
    pin.attemptedRates.clear();
    const DWORD beforeBytes = bytes; const UINT32 beforeStride = stride; const int beforeFps = fps;
    hr = ConfigureVideoPin(&pin, 1920, 1080, 120, VideoPixelFormat::Nv12,
                           bytes, stride, fps, format, nullptr);
    Check(FAILED(hr) && pin.attemptedRates == std::vector<int>{120} &&
          bytes == beforeBytes && stride == beforeStride && fps == beforeFps,
          "rejected interval candidate cannot fall back to zero timing or change output parameters");
    pin.rejectedRate = 0;

    pin.modes = {{60}};
    pin.modes[0].emptyFormat = true;
    modes = ProbePixelFormats(&pin, 1920, 1080, &query);
    std::printf("PROBE malformed-supported-entry: hr=%08X modes=%zu\n", unsigned(query), modes.size());
    Check(FAILED(query),
          "fault: malformed known video type must be distinguished from genuinely unsupported modes");

    pin.modes = {{60}};
    pin.modes[0].imageBytes = 1;
    modes = ProbePixelFormats(&pin, 1920, 1080, &query);
    std::printf("PROBE impossible-payload-list: hr=%08X modes=%zu\n", unsigned(query), modes.size());
    Check(modes.empty() || FAILED(query),
          "fault: impossible raw payload should not be advertised as an ordinary usable mode");
    pin.modes.push_back({120});
    modes = ProbePixelFormats(&pin, 1920, 1080, &query);
    Check(FAILED(query) && modes.size() == 1 && HasRate(modes, 120),
          "malformed entry does not hide a usable sibling mode");
    pin.modes = {{60}};
    pin.modes[0].subtype = GUID_NULL;
    modes = ProbePixelFormats(&pin, 1920, 1080, &query);
    Check(SUCCEEDED(query) && modes.empty(),
          "unknown unsupported subtype is not mislabeled as corrupt device data");
    pin.modes = {{0}};
    modes = ProbePixelFormats(&pin, 1920, 1080, &query);
    Check(FAILED(query) && modes.empty(),
          "absent default timing without a usable interval has explicit diagnostic status");

    pin.nullActiveFormat = true;
    AM_MEDIA_TYPE* active = nullptr;
    hr = GetActiveVideoPinFormat(&pin, &active);
    std::printf("PROBE successful-null-active: hr=%08X null=%d\n", unsigned(hr), active == nullptr);
    Check(FAILED(hr), "fault: successful GetFormat with null data must become a clear failure");
    pin.nullActiveFormat = false; pin.modes = {{60}};
    hr = GetActiveVideoPinFormat(&pin, &active);
    Check(SUCCEEDED(hr) && active != nullptr, "valid GetFormat recovers after a null response");
    if (active) { CoTaskMemFree(active->pbFormat); CoTaskMemFree(active); }
    Check(pin.references == 1, "fault probes release every acquired COM interface");
    std::printf("Fault probe assertions failed: %d\n", failures - before);
    return failures != before ? 1 : 0;
}

static int RunVideoLayoutStress() {
    using namespace llcv::video;
    const int before = failures;
    uint64_t checked = 0;
    for (unsigned seed : {1u, 17u, 20260911u, 0xdeadbeefu}) {
        std::mt19937 random(seed);
        for (int i = 0; i < 5000; ++i) {
            const GUID subtypes[]{MEDIASUBTYPE_NV12, MEDIASUBTYPE_YUY2, MFVideoFormat_P010, MEDIASUBTYPE_MJPG};
            const auto subtype = subtypes[random() % 4];
            const auto format = PixelFormatFromSubtype(subtype);
            const int width = 2 * (1 + random() % 2048);
            const int height = 2 * (1 + random() % 1080);
            const unsigned rowCount = format == VideoPixelFormat::Yuy2 ? height : height * 3 / 2;
            const unsigned pitch = width * (format == VideoPixelFormat::Nv12 ? 1 : 2) + 2 * (random() % 129);
            VIDEOINFOHEADER2 info{};
            info.bmiHeader.biWidth = width;
            info.bmiHeader.biHeight = (random() & 1) ? height : -height;
            info.bmiHeader.biSizeImage = pitch * rowCount;
            info.AvgTimePerFrame = 10'000'000 / (1 + random() % 240);
            AM_MEDIA_TYPE media{};
            media.majortype = MEDIATYPE_Video; media.subtype = subtype;
            media.formattype = FORMAT_VideoInfo2;
            media.cbFormat = sizeof(info); media.pbFormat = reinterpret_cast<BYTE*>(&info);
            DWORD bytes = 123; UINT32 stride = 456; int fps = 60;
            Check(SUCCEEDED(ValidateVideoLayout(&media, width, height, format, bytes, stride, fps)),
                  "stress: valid padded layout accepted");
            Check(format == VideoPixelFormat::Mjpeg ? stride == 0 : stride == pitch,
                  "stress: valid row padding preserved exactly");

            const DWORD savedBytes = bytes; const UINT32 savedStride = stride; const int savedFps = fps;
            switch (random() % 5) {
            case 0: media.pbFormat = nullptr; break;
            case 1: media.cbFormat = sizeof(info) - 1; break;
            case 2: info.bmiHeader.biHeight = (std::numeric_limits<LONG>::min)(); break;
            case 3: info.bmiHeader.biWidth = width + 2; break;
            default: info.AvgTimePerFrame = -1; break;
            }
            Check(FAILED(ValidateVideoLayout(&media, width, height, format, bytes, stride, fps)),
                  "stress: malformed or mismatched format rejected");
            Check(bytes == savedBytes && stride == savedStride && fps == savedFps,
                  "stress: rejected format leaves prior outputs unchanged");
            ++checked;
        }
    }
    std::printf("Layout stress: %llu valid/malformed pairs, four seeds, failures=%d\n",
        static_cast<unsigned long long>(checked), failures - before);
    return failures != before ? 1 : 0;
}
