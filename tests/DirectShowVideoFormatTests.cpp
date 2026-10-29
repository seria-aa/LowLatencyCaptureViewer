#include "video/DirectShowVideoFormat.h"
#include "video/HdrPolicy.h"

#include <dvdmedia.h>
#include <dxva.h>
#include <mfapi.h>
#include "video/VideoColor.h"

#include <cstdio>
#include <random>
#include <string>
#include <limits>
#include "FakeVideoPin.h"

namespace {

int failures = 0;

void Check(bool condition, const char* message) {
    if (condition) return;
    std::fprintf(stderr, "FAILED: %s\n", message);
    ++failures;
}

void TestPixelFormatHelpers() {
    using llcv::settings::VideoPixelFormat;
    static_assert(static_cast<int>(VideoPixelFormat::Auto) == 0 &&
                  static_cast<int>(VideoPixelFormat::Nv12) == 1 &&
                  static_cast<int>(VideoPixelFormat::Yuy2) == 2 &&
                  static_cast<int>(VideoPixelFormat::Mjpeg) == 3 &&
                  static_cast<int>(VideoPixelFormat::P010) == 4 &&
                  static_cast<int>(VideoPixelFormat::Uyvy) == 5 &&
                  static_cast<int>(VideoPixelFormat::Rgb32) == 6 &&
                  static_cast<int>(VideoPixelFormat::Rgb24) == 7);
    Check(llcv::video::PixelFormatFromSubtype(MEDIASUBTYPE_NV12) ==
              VideoPixelFormat::Nv12,
          "NV12 subtype");
    Check(llcv::video::PixelFormatFromSubtype(MEDIASUBTYPE_YUY2) ==
              VideoPixelFormat::Yuy2,
          "YUY2 subtype");
    Check(llcv::video::PixelFormatFromSubtype(MEDIASUBTYPE_UYVY) == VideoPixelFormat::Uyvy &&
              llcv::video::PixelFormatFromSubtype(llcv::video::kMediaSubtypeHdyc) == VideoPixelFormat::Uyvy,
          "UYVY and HDYC share packed 4:2:2 byte ordering");
    Check(llcv::video::PixelFormatFromSubtype(MFVideoFormat_P010) ==
              VideoPixelFormat::P010,
          "P010 subtype");
    Check(llcv::video::PixelFormatFromSubtype(MEDIASUBTYPE_MJPG) ==
              VideoPixelFormat::Mjpeg,
          "MJPEG subtype");
    Check(llcv::video::IsCompressedVideoFormat(VideoPixelFormat::Mjpeg),
          "MJPEG is compressed");
    Check(!llcv::video::IsCompressedVideoFormat(VideoPixelFormat::Nv12),
          "NV12 is not compressed");
    Check(llcv::video::IsAutoSelectableVideoFormat(VideoPixelFormat::Nv12),
          "NV12 is auto selectable");
    Check(llcv::video::IsAutoSelectableVideoFormat(VideoPixelFormat::Yuy2),
          "YUY2 is auto selectable");
    Check(llcv::video::IsAutoSelectableVideoFormat(VideoPixelFormat::Uyvy) &&
          !llcv::video::IsCompressedVideoFormat(VideoPixelFormat::Uyvy),
          "UYVY is an uncompressed automatic fallback");
    Check(!llcv::video::IsAutoSelectableVideoFormat(VideoPixelFormat::Mjpeg),
          "MJPEG remains opt-in");
}

void TestVideoInfo2Details() {
    VIDEOINFOHEADER2 info{};
    info.AvgTimePerFrame = 166'833;
    info.bmiHeader.biWidth = 2560;
    info.bmiHeader.biHeight = -1440;
    info.bmiHeader.biSizeImage = 5'529'600;

    AM_MEDIA_TYPE mediaType{};
    mediaType.majortype = MEDIATYPE_Video;
    mediaType.subtype = MEDIASUBTYPE_NV12;
    mediaType.formattype = FORMAT_VideoInfo2;
    mediaType.cbFormat = sizeof(info);
    mediaType.pbFormat = reinterpret_cast<BYTE*>(&info);

    int width = 0;
    int height = 0;
    REFERENCE_TIME duration = 0;
    DWORD imageBytes = 0;
    llcv::settings::VideoPixelFormat format =
        llcv::settings::VideoPixelFormat::Auto;
    Check(llcv::video::VideoFormatDetails(
              &mediaType, width, height, duration, imageBytes, &format),
          "VIDEOINFOHEADER2 details accepted");
    Check(width == 2560 && height == 1440, "dimensions and top-down height");
    Check(duration == 166'833, "frame duration");
    Check(imageBytes == 5'529'600, "image byte count");
    Check(format == llcv::settings::VideoPixelFormat::Nv12,
          "detected pixel format");

    mediaType.subtype = GUID_NULL;
    Check(!llcv::video::VideoFormatDetails(
              &mediaType, width, height, duration, imageBytes, &format),
          "unsupported subtype rejected");
}

void TestColorMetadataMerge() {
    llcv::video::CaptureColorMetadata base{};
    base.present = true;
    base.nominalRange = 1;
    base.transferMatrix = 4;
    base.primaries = 2;

    llcv::video::CaptureColorMetadata overrideValues{};
    overrideValues.present = true;
    overrideValues.controlFlags = 0x1234;
    overrideValues.nominalRange = 2;
    overrideValues.transferMatrix = 0;
    overrideValues.primaries = 9;
    overrideValues.transferFunction = 15;
    llcv::video::MergeDirectShowColorMetadata(base, overrideValues);

    Check(base.present, "merged metadata present");
    Check(base.controlFlags == 0x1234, "control flags replaced");
    Check(base.nominalRange == 2, "nonzero range replaced");
    Check(base.transferMatrix == 4, "zero override preserves matrix");
    Check(base.primaries == 9, "primaries replaced");
    Check(base.transferFunction == 15, "transfer function replaced");
    Check(base.hdr10(), "HDR10 candidate retained");
}

std::wstring selectionLog;
void SelectionLog(const wchar_t* message) { selectionLog += message; }

bool HasRate(const std::vector<llcv::video::PixelFormatSupport>& modes, int fps) {
    return std::any_of(modes.begin(), modes.end(), [fps](const auto& mode) {
        return mode.format == llcv::settings::VideoPixelFormat::Nv12 && mode.selectedFps == fps;
    });
}

void TestChangingDriverCapabilities() {
    using namespace llcv::video;
    std::mt19937 random(20260910);
    // Deterministic fault injection, not 1,000 independent hardware tests.
    for (int iteration = 0; iteration < 1000; ++iteration) {
        FakeVideoPin pin;
        pin.modes = {{60}, {120}, {144}, {240}, {60}, {60, 1280, 720}};
        std::shuffle(pin.modes.begin(), pin.modes.end(), random);
        const auto normal = ProbePixelFormats(&pin, 1920, 1080);
        Check(normal.size() == 4 && HasRate(normal, 240) && HasRate(normal, 144) &&
                  HasRate(normal, 120) && HasRate(normal, 60),
              "explicit high rates survive ordering, duplicates and other resolutions");

        // Same endpoint and resolution; only the fake driver's response changes.
        pin.modes = {{60}};
        auto reduced = ProbePixelFormats(&pin, 1920, 1080);
        Check(reduced.size() == 1 && HasRate(reduced, 60),
              "driver's reduced response is not replaced by stale high-rate data");
        pin.modes = {{60}, {120}, {144}, {240}};
        const int failedIndex = 1 + static_cast<int>(random() % 3);
        pin.modes[failedIndex].queryFails = true;
        auto partial = ProbePixelFormats(&pin, 1920, 1080);
        Check(partial.size() == 3 && !HasRate(partial, pin.modes[failedIndex].fps),
              "individual capability query failure silently omits that mode");
        pin.modes[failedIndex].queryFails = false;
        Check(ProbePixelFormats(&pin, 1920, 1080).size() == 4,
              "fresh successful query restores all rates");
        pin.countFails = true;
        Check(ProbePixelFormats(&pin, 1920, 1080).empty(),
              "failed capability count produces no modes");
        pin.countFails = false;
        Check(ProbePixelFormats(&pin, 1920, 1080).size() == 4 && pin.references == 1,
              "recovery does not leak the stream-config COM interface");
    }

    FakeVideoPin pin;
    pin.modes = {{60, 1920, 1080, 240}};
    auto ranged = ProbePixelFormats(&pin, 1920, 1080);
    Check(HasRate(ranged, 60) && HasRate(ranged, 120) && HasRate(ranged, 240) && !HasRate(ranged, 30),
          "high rates inside interval range are exposed without adding out-of-range rates");
    DWORD bytes = 0;
    UINT32 stride = 0;
    int actualFps = 0;
    VideoPixelFormat actualFormat = VideoPixelFormat::Auto;
    auto configure = [&] {
        selectionLog.clear(); pin.attemptedRates.clear();
        return ConfigureVideoPin(&pin, 1920, 1080, 120, VideoPixelFormat::Nv12,
                                 bytes, stride, actualFps, actualFormat, SelectionLog);
    };
    Check(SUCCEEDED(configure()) && actualFps == 120 &&
              pin.attemptedRates == std::vector<int>{120},
          "range-only requested 120 is attempted and requires driver acceptance");
    pin.rejectedRate = 120;
    Check(SUCCEEDED(configure()) && actualFps == 60 &&
              pin.attemptedRates == std::vector<int>({120, 60}),
          "range-only refusal preserves original 60fps fallback candidate");
    pin.modes = {{120}, {60}};
    pin.rejectedRate = 120;
    Check(SUCCEEDED(configure()) && actualFps == 60 &&
              pin.attemptedRates == std::vector<int>({120, 60}) &&
              selectionLog.find(L"SetFormat/fps index=120 result=0x80004005") != std::wstring::npos &&
              selectionLog.find(L"requested-rate-not-advertised") == std::wstring::npos,
          "advertised 120 rejection is distinct from missing capability in logs");
    pin.modes = {{60}};
    pin.rejectedRate = 0;
    Check(SUCCEEDED(configure()) && actualFps == 60 &&
              selectionLog.find(L"requested-rate-not-advertised") != std::wstring::npos,
          "missing 120 is explicitly identified in selection log");
    pin.modes = {{120}, {60}};
    Check(SUCCEEDED(configure()) && actualFps == 120 &&
              selectionLog.find(L"SetFormat/fps index=120 result=0x00000000") != std::wstring::npos,
          "successful negotiation returns to requested 120 after recovery");
    pin.modes = {{60}}; pin.rejectedRate = 60;
    Check(FAILED(configure()), "all rejected modes return failure rather than false success");
    pin.modes.clear();
    Check(FAILED(configure()) && pin.references == 1, "empty capabilities fail without COM leak");
    std::puts("Capability replay: 1000 seeded normal/reduced/partial/failure/recovery cycles passed;");
    std::puts("range-only enumeration/selection and distinct failure logging passed.");
}

void TestExplicitFrameRates() {
    using namespace llcv::video;
    for (const int requested : {30,40}) {
    for (const auto format : {VideoPixelFormat::Nv12, VideoPixelFormat::Yuy2,
                              VideoPixelFormat::P010, VideoPixelFormat::Mjpeg}) {
        FakeVideoPin pin;
        pin.modes = {{60}};
        auto& mode = pin.modes.front();
        mode.subtype = format == VideoPixelFormat::Nv12 ? MEDIASUBTYPE_NV12 :
            format == VideoPixelFormat::Yuy2 ? MEDIASUBTYPE_YUY2 :
            format == VideoPixelFormat::P010 ? MFVideoFormat_P010 : MEDIASUBTYPE_MJPG;
        mode.imageBytes = format == VideoPixelFormat::P010 ? 1920*1080*3 :
            format == VideoPixelFormat::Yuy2 ? 1920*1080*2 : 1920*1080*3/2;
        pin.echoAcceptedFormat = true;
        DWORD bytes = 0; UINT32 stride = 0; int fps = 0;
        VideoPixelFormat actual{};
        auto configure = [&](bool enabled) {
            pin.attemptedRates.clear();
            return ConfigureVideoPin(&pin,1920,1080,requested,format,
                bytes,stride,fps,actual,nullptr,enabled,nullptr,enabled);
        };
        Check(SUCCEEDED(configure(true)) && fps == requested && actual == format &&
            pin.attemptedRates == std::vector<int>{requested},
            "opt-in negotiates unlisted 30/40fps for each supported pixel format");
        const auto caps = ProbePixelFormats(&pin,1920,1080);
        Check(caps.size()==1 && caps.front().selectedFps==60,
            "40fps request never contaminates advertised capabilities");
        pin.rejectedRate = requested;
        Check(FAILED(configure(true)) && pin.attemptedRates==std::vector<int>{requested},
            "40fps refusal cannot fall back to 60fps");
        pin.rejectedRate = 0; pin.echoAcceptedFormat = false;
        Check(configure(true)==kRequestedFrameRateMismatch, "driver silently returning 60fps is identified");
        pin.nullActiveFormat = true;
        Check(FAILED(configure(true)), "missing driver readback is rejected");
        pin.nullActiveFormat = false;
        Check(SUCCEEDED(configure(false)) && fps==60,
            "disabled opt-in preserves legacy negotiation and fallback");
        Check(pin.references==1,"40fps negotiation releases COM references");
    }
    }
    VIDEOINFOHEADER2 info{};
    info.bmiHeader.biWidth=1920; info.bmiHeader.biHeight=1080;
    AM_MEDIA_TYPE media{};
    media.majortype=MEDIATYPE_Video; media.subtype=MEDIASUBTYPE_NV12;
    media.formattype=FORMAT_VideoInfo2; media.cbFormat=sizeof(info);
    media.pbFormat=reinterpret_cast<BYTE*>(&info);
    for (REFERENCE_TIME duration : {0LL,166667LL,248999LL,251001LL,333333LL}) {
        info.AvgTimePerFrame=duration;
        Check(!IsRequestedFrameRate(&media,40),"connected timing outside 40fps tolerance is rejected");
    }
    for (REFERENCE_TIME duration : {249000LL,250000LL,251000LL}) {
        info.AvgTimePerFrame=duration;
        Check(IsRequestedFrameRate(&media,40),"valid 40fps connected timing accepted");
    }
    for (REFERENCE_TIME duration : {332333LL,333333LL,333667LL,334333LL}) {
        info.AvgTimePerFrame=duration;
        Check(IsRequestedFrameRate(&media,30),"30fps and 29.97 readback accepted");
    }
    for (REFERENCE_TIME duration : {0LL,166667LL,250000LL,332332LL,334334LL}) {
        info.AvgTimePerFrame=duration;
        Check(!IsRequestedFrameRate(&media,30),"30fps rejects missing timing or a substituted rate");
    }
}

void TestStrictListedFrameRates() {
    using namespace llcv::video;
    for (const int requested : {30, 40, 60, 90, 100, 120, 144, 165, 180, 200, 240}) {
        for (const auto subtype : {MEDIASUBTYPE_NV12, MEDIASUBTYPE_YUY2,
                                    MFVideoFormat_P010, MEDIASUBTYPE_MJPG}) {
            for (const auto dimensions : {SIZE{1920,1080}, SIZE{2560,1440}, SIZE{3840,2160}}) {
                FakeVideoPin pin;
                pin.modes = {{requested, dimensions.cx, dimensions.cy}};
                pin.modes[0].subtype = subtype;
                pin.modes[0].imageBytes = 0;
                pin.echoAcceptedFormat = true;
                DWORD bytes = 0; UINT32 stride = 0; int fps = 0;
                VideoPixelFormat actual{};
                Check(SUCCEEDED(ConfigureVideoPin(&pin, dimensions.cx, dimensions.cy,
                    requested, PixelFormatFromSubtype(subtype), bytes, stride, fps,
                    actual, nullptr, true)) && fps == requested &&
                    actual == PixelFormatFromSubtype(subtype) &&
                    pin.attemptedRates == std::vector<int>{requested} &&
                    pin.formatQueries == 1 && pin.references == 1,
                    "explicit listed rates are strictly read back across formats/resolutions");
            }
        }
    }

    FakeVideoPin pin;
    pin.echoAcceptedFormat = true;
    DWORD bytes = 123; UINT32 stride = 456; int fps = 77;
    VideoPixelFormat format = VideoPixelFormat::Mjpeg;
    const auto configure = [&](bool strict = true, bool unlisted = false) {
        pin.attemptedRates.clear();
        return ConfigureVideoPin(&pin, 1920, 1080, 60, VideoPixelFormat::Auto,
            bytes, stride, fps, format, SelectionLog, strict, nullptr, unlisted);
    };
    pin.modes = {{120}};
    Check(FAILED(configure()) && pin.attemptedRates.empty() &&
        bytes == 123 && stride == 456 && fps == 77 && format == VideoPixelFormat::Mjpeg,
        "explicit 60 cannot fall back to advertised 120 or force an unlisted mode");
    pin.modes.push_back({60});
    pin.modes[1].subtype = MEDIASUBTYPE_YUY2;
    pin.modes[1].imageBytes = 1920 * 1080 * 2;
    Check(SUCCEEDED(configure()) && fps == 60 && format == VideoPixelFormat::Yuy2 &&
        pin.attemptedRates == std::vector<int>{60},
        "explicit 60 chooses available YUY2 60 instead of preferred NV12 120");
    Check(SUCCEEDED(configure(true, true)) && fps == 60 && format == VideoPixelFormat::Yuy2 &&
        pin.attemptedRates == std::vector<int>{60},
        "advertised exact-rate format wins over an unlisted NV12 timing probe");
    Check(SUCCEEDED(configure(false)) && fps == 120 && format == VideoPixelFormat::Nv12,
        "automatic rate retains its original preferred-format fallback policy");
    pin.rejectedRate = 60;
    Check(FAILED(configure()) && pin.attemptedRates == std::vector<int>{60},
        "explicit 60 driver refusal never tries the available 120 fallback");
    pin.rejectedRate = 0;
    pin.echoAcceptedFormat = false;
    Check(configure() == kRequestedFrameRateMismatch &&
        pin.attemptedRates == std::vector<int>{60},
        "successful SetFormat followed by substituted 120 timing fails explicitly");
    pin.nullActiveFormat = true;
    Check(FAILED(configure()), "strict listed FPS refuses a successful but null readback");
    pin.nullActiveFormat = false;
    pin.modes = {{60, 1280, 720}, {60}};
    bytes = 123; stride = 456; fps = 77; format = VideoPixelFormat::Mjpeg;
    Check(FAILED(configure()) && bytes == 123 && stride == 456 && fps == 77 &&
        format == VideoPixelFormat::Mjpeg && pin.attemptedRates == std::vector<int>{60},
        "matching FPS readback cannot conceal a substituted resolution or publish partial outputs");
    pin.modes = {{60}, {60}};
    pin.modes.front().subtype = MEDIASUBTYPE_YUY2;
    pin.modes.front().imageBytes = 1920 * 1080 * 2;
    pin.attemptedRates.clear();
    Check(FAILED(ConfigureVideoPin(&pin, 1920, 1080, 60, VideoPixelFormat::Nv12,
        bytes, stride, fps, format, nullptr, true)) && bytes == 123 && stride == 456 &&
        fps == 77 && format == VideoPixelFormat::Mjpeg,
        "matching FPS readback cannot conceal a substituted pixel format");
    pin.echoAcceptedFormat = true;
    pin.modes = {{120}};
    pin.modes.front().minimumInterval = 83333;
    pin.modes.front().maximumInterval = 166667;
    Check(SUCCEEDED(configure()) && fps == 60 && pin.attemptedRates == std::vector<int>{60},
        "strict listed rate permits explicit intervals within advertised caps");
    pin.modes.front().minimumInterval = 0;
    pin.modes.front().maximumInterval = 0;
    Check(FAILED(configure()) && pin.attemptedRates.empty(),
        "strict rate does not treat support in a previous caps response as current support");
    Check(configure(false, true) == E_INVALIDARG && pin.attemptedRates.empty(),
        "unlisted probes cannot be requested without strict verification");
    Check(pin.references == 1, "strict negotiation failures do not leak the config interface");

    VIDEOINFOHEADER2 info{};
    info.bmiHeader.biWidth = 1920; info.bmiHeader.biHeight = 1080;
    AM_MEDIA_TYPE media{};
    media.majortype = MEDIATYPE_Video; media.subtype = MEDIASUBTYPE_NV12;
    media.formattype = FORMAT_VideoInfo2; media.cbFormat = sizeof(info);
    media.pbFormat = reinterpret_cast<BYTE*>(&info);
    for (const int requested : {30, 40, 60, 90, 100, 120, 144, 165, 180, 200, 240}) {
        info.AvgTimePerFrame = 10'000'000 / requested;
        Check(IsRequestedFrameRate(&media, requested), "strict timing accepts nominal integer FPS");
        info.AvgTimePerFrame = 10'010'000 / requested;
        Check(IsRequestedFrameRate(&media, requested), "strict timing accepts 1000/1001 fractional FPS");
        for (const int substituted : {requested - 1, requested + 1, requested * 2}) {
            info.AvgTimePerFrame = 10'000'000 / substituted;
            Check(!IsRequestedFrameRate(&media, requested),
                "strict timing rejects neighboring rates and doubled rate even at high FPS");
        }
        info.AvgTimePerFrame = 0;
        Check(!IsRequestedFrameRate(&media, requested), "strict timing rejects omitted duration");
    }
    for (const int invalid : {-1, 0, 10'000'001, (std::numeric_limits<int>::max)()})
        Check(!IsRequestedFrameRate(&media, invalid), "strict timing rejects invalid requested FPS");
}

void TestAcceptedCandidateColorMetadata() {
    using namespace llcv::video;
    // DXVA extended color data: left chroma, studio range, BT.2020 NCL/PQ.
    constexpr DWORD pqLeft = AMCONTROL_COLORINFO_PRESENT |
        (5u << 8) | (2u << 12) | (4u << 15) | (9u << 22) | (15u << 27);
    constexpr DWORD hlgLeft = (pqLeft & ~(31u << 27)) | (16u << 27);
    const auto modeWithColor = [](DWORD flags) {
        FakeVideoPin::Mode mode{60};
        mode.subtype = MFVideoFormat_P010;
        mode.imageBytes = 1920 * 1080 * 3;
        mode.controlFlags = flags;
        return mode;
    };
    DWORD bytes = 0;
    UINT32 stride = 0;
    int fps = 0;
    VideoPixelFormat format{};
    CaptureColorMetadata selected{};
    auto configure = [&](FakeVideoPin& pin, bool request40 = true) {
        return ConfigureVideoPin(&pin, 1920, 1080, request40 ? 40 : 60,
            VideoPixelFormat::P010, bytes, stride, fps, format, nullptr,
            request40, &selected, request40);
    };

    for (const DWORD flags : {pqLeft, hlgLeft}) {
        for (const bool legacy : {false, true}) {
            FakeVideoPin pin;
            pin.modes = {modeWithColor(flags)};
            pin.echoAcceptedFormat = true;
            pin.stripReadbackColor = true;
            pin.legacyReadback = legacy;
            Check(SUCCEEDED(configure(pin)) && fps == 40 && selected.present &&
                selected.controlFlags == flags && selected.chromaSubsampling == 5 &&
                selected.transferFunction == (flags == pqLeft ? 15u : 16u),
                "unlisted 40fps retains exact accepted caps-only PQ/HLG color hints");
            Check(pin.formatQueries == 1,
                "retaining color does not add a driver query to strict negotiation");
            CaptureColorMetadata matched{};
            Check(!FindMatchingDirectShowColorMetadata(&pin, VideoPixelFormat::P010,
                1920, 1080, 40, matched),
                "reproduction: exact-rate caps lookup cannot recover retimed color hints");
            AM_MEDIA_TYPE* active = nullptr;
            Check(SUCCEEDED(GetActiveVideoPinFormat(&pin, &active)),
                "query stripped active format after selecting 40fps");
            CaptureColorMetadata activeColor{};
            Check(!ExtractDirectShowColorMetadata(active, activeColor),
                "reproduction: readback omits color for both header styles");
            MergeDirectShowColorMetadata(selected, activeColor);
            const auto resolved = llcv::hdr::ResolveInput(selected, false);
            if (flags == pqLeft) {
                Check(resolved.kind == llcv::hdr::InputKind::Hdr10 && !resolved.assumed &&
                    resolved.colorSpace == DXGI_COLOR_SPACE_YCBCR_STUDIO_G2084_LEFT_P2020,
                    "retimed PQ preserves left chroma instead of assuming top-left");
            } else {
                Check(resolved.kind == llcv::hdr::InputKind::Unsupported,
                    "retimed HLG remains unsupported instead of being assumed HDR10");
            }
            if (active) {
                if (active->pUnk) active->pUnk->Release();
                CoTaskMemFree(active->pbFormat);
                CoTaskMemFree(active);
            }
            // The original explicitly advertised mode still has the same color
            // and no new readback query in ordinary negotiation.
            const int queriesBefore = pin.formatQueries;
            Check(SUCCEEDED(configure(pin, false)) && fps == 60 &&
                selected.controlFlags == flags && pin.formatQueries == queriesBefore,
                "normal 60fps selection retains color without extra driver work");
            Check(pin.references == 1, "caps-only color tests release COM references");
        }
    }

    FakeVideoPin pin;
    pin.echoAcceptedFormat = true;
    pin.stripReadbackColor = true;
    pin.modes = {modeWithColor(pqLeft), modeWithColor(hlgLeft)};
    pin.rejectedControlFlags = pqLeft;
    Check(SUCCEEDED(configure(pin)) && selected.controlFlags == hlgLeft &&
        pin.attemptedRates == std::vector<int>({40, 40}),
        "rejected candidate color cannot replace a later accepted candidate");
    pin.modes[1].controlFlags = 0;
    Check(SUCCEEDED(configure(pin)) && !selected.present && selected.controlFlags == 0,
        "a later untagged accepted candidate does not inherit rejected color");
    pin.modes = {modeWithColor(pqLeft)};
    selected.present = true; selected.controlFlags = hlgLeft;
    Check(FAILED(configure(pin)) && !selected.present && selected.controlFlags == 0,
        "SetFormat refusal clears previous selected metadata");
    pin.rejectedControlFlags = 0;
    pin.echoAcceptedFormat = false;
    selected.present = true; selected.controlFlags = hlgLeft;
    Check(FAILED(configure(pin)) && !selected.present && selected.controlFlags == 0,
        "readback refusal does not expose the rejected candidate's color");
    selected.present = true; selected.controlFlags = hlgLeft;
    Check(ConfigureVideoPin(nullptr, 1920, 1080, 40, VideoPixelFormat::P010,
        bytes, stride, fps, format, nullptr, true, &selected) == E_POINTER &&
        !selected.present && selected.controlFlags == 0,
        "invalid call clears selected metadata before returning");
    Check(pin.references == 1, "failed metadata selections release COM references");
}

void TestSelectedVersusActiveFormat() {
    using namespace llcv::video;
    FakeVideoPin pin;
    for (int iteration = 0; iteration < 100; ++iteration) {
        pin.modes = {{120}};
        DWORD bytes = 0;
        UINT32 stride = 0;
        int fps = 0;
        VideoPixelFormat format{};
        Check(SUCCEEDED(ConfigureVideoPin(&pin, 1920, 1080, 120,
                    VideoPixelFormat::Nv12, bytes, stride, fps, format, nullptr)) &&
                  bytes == 3110400 && stride == 1920 && fps == 120,
              "selected NV12 FHD layout uses expected bytes and stride");
        // A deliberately inconsistent driver response after successful SetFormat.
        pin.modes = {{60, 1280, 720}};
        AM_MEDIA_TYPE* active = nullptr;
        Check(SUCCEEDED(GetActiveVideoPinFormat(&pin, &active)),
              "active-format query can succeed even when driver changes the type");
        int width = 0, height = 0;
        REFERENCE_TIME duration = 0;
        DWORD activeBytes = 0;
        Check(VideoFormatDetails(active, width, height, duration, activeBytes) &&
                  width == 1280 && height == 720 && activeBytes < bytes && stride == 1920,
              "characterization: active query does not reconcile previous layout outputs");
        Check(FAILED(ValidateVideoLayout(active, 1920, 1080, VideoPixelFormat::Nv12,
                                        bytes, stride, fps)) && stride == 1920 && bytes == 3110400,
              "startup validation now rejects changed dimensions without corrupting prior outputs");
        if (active) {
            if (active->pUnk) active->pUnk->Release();
            CoTaskMemFree(active->pbFormat); CoTaskMemFree(active);
        }
    }
    for (int padding : {0, 64, 128, 256}) {
        pin.modes = {{60}};
        pin.modes[0].imageBytes = (1920 + padding) * 1080 * 3 / 2;
        DWORD bytes = 0;
        UINT32 stride = 0;
        int fps = 0;
        VideoPixelFormat format{};
        Check(SUCCEEDED(ConfigureVideoPin(&pin, 1920, 1080, 60,
                    VideoPixelFormat::Nv12, bytes, stride, fps, format, nullptr)) &&
                  stride == static_cast<UINT32>(1920 + padding),
              "declared NV12 row padding is retained in upload stride");
    }
    Check(pin.references == 1, "active-format replay releases stream-config interfaces");
}

void TestLayoutBoundariesAndProbeErrors() {
    using namespace llcv::video;
    for (const auto& subtype : {MEDIASUBTYPE_NV12, MEDIASUBTYPE_YUY2, MFVideoFormat_P010, MEDIASUBTYPE_MJPG}) {
        const auto format = PixelFormatFromSubtype(subtype);
        FakeVideoPin pin;
        pin.modes = {{60}};
        pin.modes[0].subtype = subtype;
        pin.modes[0].imageBytes = 0; // uncompressed drivers may omit biSizeImage
        AM_MEDIA_TYPE* active = nullptr;
        Check(SUCCEEDED(GetActiveVideoPinFormat(&pin, &active)), "query boundary media type");
        DWORD bytes = 123;
        UINT32 stride = 456;
        int fps = 7;
        Check(SUCCEEDED(ValidateVideoLayout(active, 1920, 1080, format, bytes, stride, fps)) && fps == 60,
              "all supported input subtypes validate with zero declared size");
        auto* timing = reinterpret_cast<VIDEOINFOHEADER2*>(active->pbFormat);
        const auto savedDuration = timing->AvgTimePerFrame;
        timing->AvgTimePerFrame = 0;
        Check(SUCCEEDED(ValidateVideoLayout(active, 1920, 1080, format, bytes, stride, fps)) && fps == 60,
              "missing connected timing preserves the negotiated frame rate");
        fps = 0;
        Check(FAILED(ValidateVideoLayout(active, 1920, 1080, format, bytes, stride, fps)),
              "missing timing without a negotiated rate is rejected");
        fps = 60;
        timing->AvgTimePerFrame = -1;
        Check(FAILED(ValidateVideoLayout(active, 1920, 1080, format, bytes, stride, fps)),
              "negative frame timing is rejected despite a fallback rate");
        timing->AvgTimePerFrame = savedDuration;
        if (format != VideoPixelFormat::Mjpeg) {
            Check(bytes == static_cast<DWORD>(format == VideoPixelFormat::Nv12 ? 3110400 :
                         format == VideoPixelFormat::Yuy2 ? 4147200 : 6220800),
                  "zero declared size is safely derived per pixel format");
            auto* info = reinterpret_cast<VIDEOINFOHEADER2*>(active->pbFormat);
            info->bmiHeader.biSizeImage = bytes - 1;
            Check(FAILED(ValidateVideoLayout(active, 1920, 1080, format, bytes, stride, fps)),
                  "declared payload smaller than required layout is rejected");
            info->bmiHeader.biSizeImage = 0;
            info->bmiHeader.biHeight = (std::numeric_limits<LONG>::min)();
            Check(FAILED(ValidateVideoLayout(active, 1920, 1080, format, bytes, stride, fps)),
                  "malformed signed height fails without abs overflow");
        }
        auto* buffer = active->pbFormat;
        active->pbFormat = nullptr;
        Check(FAILED(ValidateVideoLayout(active, 1920, 1080, format, bytes, stride, fps)),
              "null media payload rejected despite nonzero cbFormat");
        CoTaskMemFree(buffer); CoTaskMemFree(active);
    }
    FakeVideoPin pin;
    HRESULT status = S_OK;
    pin.countFails = true;
    Check(ProbePixelFormats(&pin, 1920, 1080, &status).empty() && FAILED(status),
          "whole query failure is distinguished from no supported modes");
    pin.countFails = false;
    Check(ProbePixelFormats(&pin, 1920, 1080, &status).empty() && SUCCEEDED(status),
          "empty successful enumeration is genuine no-mode result");
    pin.modes = {{60}, {120}};
    pin.modes[1].queryFails = true;
    selectionLog.clear();
    const auto modes = ProbePixelFormats(&pin, 1920, 1080, &status, SelectionLog);
    Check(modes.size() == 1 && FAILED(status) &&
          selectionLog.find(L"probe/entry index=1 result=0x80004005") != std::wstring::npos,
          "partial enumeration failure retains usable modes plus diagnostic status");
    pin.modes[1].queryFails = false;
    Check(ProbePixelFormats(&pin, 1920, 1080, &status).size() == 2 && SUCCEEDED(status),
          "recovery clears prior partial error state");
}

void TestMalformedCapabilityOwnership() {
    using namespace llcv::video;
    FakeVideoPin pin;
    pin.modes = {{60}};
    pin.modes.front().queryFails = true;
    pin.allocateOnQueryFailure = true;
    const auto checkMediaReleased = [&](const char* message) {
        Check(pin.lastFailedMedia != nullptr, "fault fixture actually returned allocated failed media");
        Check(pin.references == 1, message);
        // Keep the fixture leak-free even when replayed against the old code.
        // The extra media-owned reference proves this pointer is still live.
        if (pin.references == 2 && pin.lastFailedMedia) {
            CoTaskMemFree(pin.lastFailedMedia->pbFormat);
            pin.lastFailedMedia->pUnk->Release();
            CoTaskMemFree(pin.lastFailedMedia);
        }
        pin.lastFailedMedia = nullptr;
    };

    CaptureColorMetadata metadata{};
    metadata.present = true;
    Check(!FindMatchingDirectShowColorMetadata(&pin, VideoPixelFormat::Nv12,
        1920, 1080, 60, metadata) && !metadata.present,
        "failed color-capability query does not expose stale metadata");
    checkMediaReleased("color metadata query releases media allocated despite failure HRESULT");

    DWORD bytes = 123; UINT32 stride = 456; int fps = 77;
    VideoPixelFormat format = VideoPixelFormat::Mjpeg;
    Check(FAILED(ConfigureVideoPin(&pin, 1920, 1080, 60, VideoPixelFormat::Nv12,
        bytes, stride, fps, format, nullptr, true)) && bytes == 123 && stride == 456 &&
        fps == 77 && format == VideoPixelFormat::Mjpeg,
        "failed allocated capability cannot publish a format selection");
    checkMediaReleased("format negotiation releases media allocated despite failure HRESULT");

    HRESULT query = S_OK;
    Check(ProbePixelFormats(&pin, 1920, 1080, &query).empty() && FAILED(query),
        "failed allocated capability remains an explicit probe failure");
    checkMediaReleased("format probe releases media allocated despite failure HRESULT");

    pin.allocateOnQueryFailure = false;
    pin.modes.front().queryFails = false;
    pin.negativeCapabilityCount = true;
    pin.streamCapsQueries = 0;
    Check(ConfigureVideoPin(&pin, 1920, 1080, 60, VideoPixelFormat::Nv12,
        bytes, stride, fps, format, nullptr, true) == E_FAIL &&
        pin.streamCapsQueries == 0 && pin.attemptedRates.empty() &&
        bytes == 123 && stride == 456 && fps == 77 && format == VideoPixelFormat::Mjpeg,
        "negative capability count is rejected before enumeration or output changes");
    Check(ProbePixelFormats(&pin, 1920, 1080, &query).empty() && query == E_FAIL &&
        pin.streamCapsQueries == 0,
        "negative probe count is malformed data rather than a successful empty list");
    metadata.present = true;
    Check(!FindMatchingDirectShowColorMetadata(&pin, VideoPixelFormat::Nv12,
        1920, 1080, 60, metadata) && !metadata.present && pin.streamCapsQueries == 0,
        "negative color-capability count is rejected without enumeration");
    Check(pin.references == 1, "malformed capability count releases the config interface");

    pin.negativeCapabilityCount = false;
    pin.echoAcceptedFormat = true;
    Check(SUCCEEDED(ConfigureVideoPin(&pin, 1920, 1080, 60, VideoPixelFormat::Nv12,
        bytes, stride, fps, format, nullptr, true)) && fps == 60 && pin.references == 1,
        "normal enumeration and selection recover after malformed capability responses");
}

void TestUnsupportedFormatDiagnostics() {
    using namespace llcv::video;
    GUID yv12 = MEDIASUBTYPE_YUY2;
    yv12.Data1 = MAKEFOURCC('Y', 'V', '1', '2');
    GUID v210 = MEDIASUBTYPE_YUY2;
    v210.Data1 = MAKEFOURCC('v', '2', '1', '0');
    FakeVideoPin pin;
    VideoCapabilityDiagnostics diagnostics;
    HRESULT status = E_FAIL;
    auto probe = [&](int width = 1920, int height = 1080) {
        selectionLog.clear();
        return ProbePixelFormats(&pin, width, height, &status, SelectionLog, &diagnostics);
    };
    Check(probe().empty() && SUCCEEDED(status) && !diagnostics.UnsupportedFormatsOnly() &&
        diagnostics.matchingResolutionTypes == 0 && diagnostics.unsupportedFormatNames.empty(),
        "successful empty list is not an unsupported-subtype verdict");

    pin.modes = {{60}, {30}, {60}, {60, 1280, 720}};
    pin.modes[0].subtype = pin.modes[1].subtype = v210;
    pin.modes[2].subtype = yv12;
    pin.modes[3].subtype = v210;
    Check(probe().empty() && SUCCEEDED(status) && diagnostics.UnsupportedFormatsOnly() &&
        diagnostics.matchingResolutionTypes == 3 && diagnostics.unsupportedMatchingTypes == 3 &&
        diagnostics.unsupportedFormatNames == std::vector<std::wstring>{L"v210", L"YV12"},
        "unsupported-only verdict is resolution scoped with unique actual FourCC names");
    Check(selectionLog.find(L"reason=unsupported-subtype subtype=v210") != std::wstring::npos &&
        selectionLog.find(L"subtype=YV12") != std::wstring::npos &&
        selectionLog.find(L"subtype=v210") != std::wstring::npos &&
        selectionLog.find(L"size=1920x1080 duration=166666 (100ns) header=valid") != std::wstring::npos &&
        selectionLog.find(L"size=1280x720") != std::wstring::npos,
        "rejected entries log FourCC, GUID, raw dimensions and frame interval even outside selection");

    pin.modes.push_back({60});
    const auto mixed = probe();
    Check(mixed.size() == 1 && mixed.front().format == VideoPixelFormat::Nv12 &&
        SUCCEEDED(status) && !diagnostics.UnsupportedFormatsOnly() &&
        diagnostics.matchingResolutionTypes == 4 && diagnostics.unsupportedMatchingTypes == 3,
        "an available recognized format prevents an unsupported-only verdict");

    pin.modes = {{60, 1280, 720}};
    pin.modes[0].subtype = v210;
    Check(probe().empty() && SUCCEEDED(status) && !diagnostics.UnsupportedFormatsOnly() &&
        diagnostics.unsupportedFormatNames.empty(),
        "unsupported types at another resolution do not misdiagnose the selected resolution");
    pin.modes.push_back({60});
    pin.modes.back().subtype = v210;
    pin.modes.front().subtype = MEDIASUBTYPE_NV12;
    Check(probe().empty() && diagnostics.UnsupportedFormatsOnly() &&
        diagnostics.unsupportedFormatNames == std::vector<std::wstring>{L"v210"} &&
        selectionLog.find(L"reason=resolution-mismatch subtype=NV12") != std::wstring::npos,
        "recognized types elsewhere do not hide unsupported selected-resolution types");

    pin.modes = {{60}};
    pin.modes[0].subtype = v210;
    pin.modes[0].subtype.Data2 = 7; // Same Data1 bytes, but not a FourCC GUID.
    Check(probe().empty() && diagnostics.UnsupportedFormatsOnly() &&
        diagnostics.unsupportedFormatNames.size() == 1 &&
        diagnostics.unsupportedFormatNames.front().front() == L'{' &&
        selectionLog.find(L"subtype=v210") == std::wstring::npos,
        "vendor GUIDs are not incorrectly described as FourCC values");

    pin.modes[0].emptyFormat = true;
    Check(probe().empty() && SUCCEEDED(status) && !diagnostics.UnsupportedFormatsOnly() &&
        selectionLog.find(L"header=unavailable") != std::wstring::npos,
        "malformed unknown headers cannot establish a resolution-specific diagnosis");
    pin.modes[0].emptyFormat = false;
    pin.modes[0].subtype = MEDIASUBTYPE_NV12;
    pin.modes[0].imageBytes = 1;
    Check(probe().empty() && FAILED(status) && !diagnostics.UnsupportedFormatsOnly() &&
        selectionLog.find(L"reason=yuv-image-size subtype=NV12") != std::wstring::npos,
        "recognized invalid layouts remain query errors rather than unsupported-only formats");

    pin.modes = {{60}, {60}};
    pin.modes[0].subtype = v210;
    pin.modes[1].queryFails = true;
    Check(probe().empty() && FAILED(status) && diagnostics.UnsupportedFormatsOnly(),
        "partial enumeration failure is retained separately from the observed format evidence");
    pin.countFails = true;
    Check(probe().empty() && FAILED(status) && !diagnostics.UnsupportedFormatsOnly() &&
        diagnostics.unsupportedFormatNames.empty(),
        "failed requery clears a stale unsupported-only diagnosis");
    Check(ProbePixelFormats(nullptr, 1920, 1080, &status, nullptr, &diagnostics).empty() &&
        status == E_POINTER && diagnostics.matchingResolutionTypes == 0 && pin.references == 1,
        "null pin clears diagnostics and capability queries preserve COM ownership");
}

void TestUyvyNegotiationAndColor() {
    using namespace llcv::video;
    namespace color = llcv::video_color;
    const auto mode = [](GUID subtype) {
        FakeVideoPin::Mode result{60};
        result.subtype = subtype;
        result.imageBytes = 1920 * 1080 * (subtype == MEDIASUBTYPE_NV12 ? 3 : 4) / 2;
        return result;
    };
    FakeVideoPin pin;
    pin.echoAcceptedFormat = true;
    pin.modes = {mode(kMediaSubtypeHdyc), mode(MEDIASUBTYPE_UYVY),
                 mode(MEDIASUBTYPE_YUY2), mode(MEDIASUBTYPE_NV12)};
    DWORD bytes = 0;
    UINT32 stride = 0;
    int fps = 0;
    VideoPixelFormat format{};
    CaptureColorMetadata selected{};
    GUID selectedSubtype{};
    const auto configure = [&](VideoPixelFormat wanted = VideoPixelFormat::Auto, int rate = 60,
                               bool unlisted = false) {
        return ConfigureVideoPin(&pin, 1920, 1080, rate, wanted, bytes, stride, fps,
                                 format, nullptr, unlisted, &selected, unlisted, &selectedSubtype);
    };
    for (const auto expected : {VideoPixelFormat::Nv12, VideoPixelFormat::Yuy2, VideoPixelFormat::Uyvy}) {
        Check(SUCCEEDED(configure()) && format == expected && fps == 60,
            "automatic preference remains NV12 then YUY2 then UYVY independent of enumeration order");
        if (expected != VideoPixelFormat::Uyvy) pin.modes.pop_back();
    }
    Check(pin.acceptedSubtype == kMediaSubtypeHdyc && selectedSubtype == kMediaSubtypeHdyc && !selected.present &&
        bytes == 1920 * 1080 * 2 && stride == 1920 * 2,
        "HDYC negotiates packed UYVY without inventing explicit device metadata");
    Check(SUCCEEDED(configure(VideoPixelFormat::Uyvy, 40, true)) && fps == 40 && !selected.present,
        "explicit unlisted UYVY timing accepts untagged HDYC without fabricating metadata");

    pin.modes = {mode(kMediaSubtypeHdyc), mode(MEDIASUBTYPE_UYVY)};
    VideoCapabilityDiagnostics diagnostics;
    HRESULT query = E_FAIL;
    const auto formats = ProbePixelFormats(&pin, 1920, 1080, &query, nullptr, &diagnostics);
    Check(formats.size() == 1 && formats.front().format == VideoPixelFormat::Uyvy &&
        SUCCEEDED(query) && !diagnostics.UnsupportedFormatsOnly() &&
        diagnostics.unsupportedMatchingTypes == 0,
        "UYVY and HDYC capabilities appear once and no longer produce unsupported diagnostics");

    constexpr DWORD bt601Full = AMCONTROL_COLORINFO_PRESENT | (2u << 15) | (1u << 12);
    constexpr DWORD bt709Limited = AMCONTROL_COLORINFO_PRESENT | (1u << 15) | (2u << 12);
    for (const auto activeSubtype : {kMediaSubtypeHdyc, MEDIASUBTYPE_UYVY}) {
        const auto otherSubtype = activeSubtype == kMediaSubtypeHdyc
            ? MEDIASUBTYPE_UYVY : kMediaSubtypeHdyc;
        for (const bool reverse : {false, true}) {
            pin.modes = {mode(activeSubtype), mode(otherSubtype)};
            pin.modes.back().controlFlags = bt601Full;
            if (reverse) std::reverse(pin.modes.begin(), pin.modes.end());
            CaptureColorMetadata capsColor{};
            capsColor.present = true; capsColor.transferMatrix = 4;
            Check(!FindMatchingDirectShowColorMetadata(&pin, VideoPixelFormat::Uyvy,
                1920, 1080, 60, capsColor, &activeSubtype) && !capsColor.present &&
                capsColor.transferMatrix == 0,
                "untagged packed-422 subtype never borrows the other alias color in either enumeration order");
            for (auto& entry : pin.modes)
                if (entry.subtype == activeSubtype) entry.controlFlags = bt709Limited;
            Check(FindMatchingDirectShowColorMetadata(&pin, VideoPixelFormat::Uyvy,
                1920, 1080, 60, capsColor, &activeSubtype) &&
                capsColor.transferMatrix == 1 && capsColor.nominalRange == 2,
                "caps fallback selects the active packed-422 subtype's own matrix and range");
            Check(FindMatchingDirectShowColorMetadata(&pin, VideoPixelFormat::Uyvy,
                1920, 1080, 60, capsColor, &otherSubtype) &&
                capsColor.transferMatrix == 2 && capsColor.nominalRange == 1,
                "opposite alias lookup retains its separate explicit color declaration");
        }

        // Reject the first candidate, accept the other alias, then model a
        // driver returning the first subtype without color in GetFormat.
        pin.modes = {mode(activeSubtype), mode(otherSubtype)};
        pin.modes.front().controlFlags = bt601Full;
        pin.modes.back().controlFlags = bt709Limited;
        pin.rejectedControlFlags = bt601Full;
        pin.echoAcceptedFormat = false;
        pin.stripReadbackColor = true;
        Check(SUCCEEDED(configure(VideoPixelFormat::Uyvy, 60, true)) &&
            pin.acceptedSubtype == otherSubtype && selectedSubtype == otherSubtype && !selected.present,
            "substituted UYVY/HDYC readback discards color belonging only to the accepted other alias");
        pin.echoAcceptedFormat = true;
        Check(SUCCEEDED(configure(VideoPixelFormat::Uyvy, 60, true)) &&
            selectedSubtype == otherSubtype && selected.present &&
            selected.transferMatrix == 1 && selected.nominalRange == 2,
            "same-subtype stripped readback still preserves accepted candidate color");
    }
    pin.rejectedControlFlags = 0;
    pin.echoAcceptedFormat = true;
    pin.stripReadbackColor = false;

    for (const auto subtype : {MEDIASUBTYPE_UYVY, kMediaSubtypeHdyc}) {
        VIDEOINFOHEADER2 info{};
        info.AvgTimePerFrame = 166'833;
        info.bmiHeader.biWidth = 1920;
        info.bmiHeader.biHeight = 1081; // Packed 4:2:2 needs even width, not even height.
        AM_MEDIA_TYPE media{};
        media.majortype = MEDIATYPE_Video; media.subtype = subtype;
        media.formattype = FORMAT_VideoInfo2; media.cbFormat = sizeof(info);
        media.pbFormat = reinterpret_cast<BYTE*>(&info);
        bytes = 0; stride = 0; fps = 60;
        Check(SUCCEEDED(ValidateVideoLayout(&media, 1920, 1081, VideoPixelFormat::Uyvy,
            bytes, stride, fps)) && bytes == 1920 * 1081 * 2 && stride == 3840 && fps == 60,
            "packed UYVY layout supports odd heights with exact two-byte pixel storage");
        info.bmiHeader.biSizeImage = (3840 + 64) * 1081;
        Check(SUCCEEDED(ValidateVideoLayout(&media, 1920, 1081, VideoPixelFormat::Uyvy,
            bytes, stride, fps)) && stride == 3904,
            "UYVY row padding is preserved");
        info.bmiHeader.biSizeImage = 1920 * 1081 * 2 - 1;
        Check(FAILED(ValidateVideoLayout(&media, 1920, 1081, VideoPixelFormat::Uyvy, bytes, stride, fps)),
            "truncated UYVY payload is rejected");
        info.bmiHeader.biWidth = 1919; info.bmiHeader.biSizeImage = 0;
        Check(FAILED(ValidateVideoLayout(&media, 1919, 1081, VideoPixelFormat::Uyvy, bytes, stride, fps)),
            "odd-width UYVY cannot split a shared chroma pair");
    }

    VIDEOINFOHEADER legacy{};
    legacy.bmiHeader.biWidth = 1920; legacy.bmiHeader.biHeight = 1080;
    AM_MEDIA_TYPE media{};
    media.majortype = MEDIATYPE_Video; media.subtype = kMediaSubtypeHdyc;
    media.formattype = FORMAT_VideoInfo; media.cbFormat = sizeof(legacy);
    media.pbFormat = reinterpret_cast<BYTE*>(&legacy);
    CaptureColorMetadata metadata{};
    Check(!ExtractDirectShowColorMetadata(&media, metadata) && !metadata.present &&
        metadata.transferMatrix == 0 && metadata.nominalRange == 0,
        "legacy HDYC does not label its BT.709 fallback as explicit device metadata");
    const auto resolved = color::Resolve(false, 1920, 1080, {},
        {metadata.transferMatrix, metadata.nominalRange}, color::MatrixOverride::Bt601,
        color::RangeOverride::Full);
    Check(resolved.matrix == color::Matrix::Bt709 && resolved.range == color::Range::Full &&
        resolved.rangeSource == color::Source::UserOverride,
        "HDYC respects SDR range override and never inherits MJPEG-only matrix override");
    media.subtype = MEDIASUBTYPE_UYVY;
    Check(!ExtractDirectShowColorMetadata(&media, metadata) && !metadata.present,
        "untagged UYVY does not pretend to have HDYC subtype metadata");

    VIDEOINFOHEADER2 info{};
    info.bmiHeader = legacy.bmiHeader;
    info.dwControlFlags = AMCONTROL_COLORINFO_PRESENT | (2u << 15) | (1u << 12);
    media.subtype = kMediaSubtypeHdyc; media.formattype = FORMAT_VideoInfo2;
    media.cbFormat = sizeof(info); media.pbFormat = reinterpret_cast<BYTE*>(&info);
    Check(ExtractDirectShowColorMetadata(&media, metadata) && metadata.transferMatrix == 2 &&
        metadata.nominalRange == 1,
        "explicit HDYC matrix/range metadata wins over the subtype default");
    const auto explicitColor = color::Resolve(false, 1920, 1080, {},
        {metadata.transferMatrix, metadata.nominalRange}, color::MatrixOverride::Bt709,
        color::RangeOverride::Limited);
    Check(explicitColor.matrix == color::Matrix::Bt601 && explicitColor.range == color::Range::Limited,
        "explicit metadata matrix and user SDR range have independent precedence");
    info.dwControlFlags = AMCONTROL_COLORINFO_PRESENT | (1u << 12);
    Check(ExtractDirectShowColorMetadata(&media, metadata) && metadata.transferMatrix == 0 &&
        metadata.nominalRange == 1 && color::Resolve(false, 1920, 1080, {},
            {metadata.transferMatrix, metadata.nominalRange}).matrix == color::Matrix::Bt709,
        "partial HDYC color resolves missing matrix to BT.709 without inventing a declaration");
    media.cbFormat = 1;
    Check(!ExtractDirectShowColorMetadata(&media, metadata) && !metadata.present,
        "truncated HDYC headers do not fabricate valid color metadata");

    for (const bool legacyReadback : {false, true}) {
        pin.modes = {mode(kMediaSubtypeHdyc)};
        pin.modes.front().controlFlags = AMCONTROL_COLORINFO_PRESENT | (2u << 15) | (1u << 12);
        pin.stripReadbackColor = true;
        pin.legacyReadback = legacyReadback;
        Check(SUCCEEDED(configure(VideoPixelFormat::Uyvy, 40, true)) && selected.present &&
            selected.transferMatrix == 2 && selected.nominalRange == 1,
            "retimed HDYC preserves explicit accepted-candidate BT.601/full hints");
        AM_MEDIA_TYPE* active = nullptr;
        Check(SUCCEEDED(GetActiveVideoPinFormat(&pin, &active)) && active,
            "HDYC driver returns an active media type after dropping color flags");
        if (!active) continue;
        CaptureColorMetadata activeColor{};
        Check(!ExtractDirectShowColorMetadata(active, activeColor),
            "stripped HDYC active metadata cannot override accepted color with a default");
        CaptureColorMetadata capsColor{};
        Check(FindMatchingDirectShowColorMetadata(&pin, VideoPixelFormat::Uyvy, 1920, 1080, 60,
            capsColor) && capsColor.transferMatrix == 2 && capsColor.nominalRange == 1,
            "advertised HDYC mode retains explicit metadata for ordinary startup fallback");
        auto effective = selected;
        MergeDirectShowColorMetadata(effective, activeColor);
        VideoSampleFormat connected{};
        Check(ReadVideoSampleFormat(active, fps, connected), "read stripped connected HDYC layout");
        MergeDirectShowColorMetadata(effective, connected.color);
        Check(effective.transferMatrix == 2 && effective.nominalRange == 1,
            "active and connected missing HDYC declarations preserve accepted explicit color");
        connected.color = effective;
        Check(MatchesVideoSampleFormat(active, connected),
            "untagged dynamic HDYC samples do not falsely trigger a BT.601 to BT.709 format change");
        if (!legacyReadback) {
            reinterpret_cast<VIDEOINFOHEADER2*>(active->pbFormat)->dwControlFlags =
                AMCONTROL_COLORINFO_PRESENT | (1u << 15) | (1u << 12);
            Check(!MatchesVideoSampleFormat(active, connected),
                "genuine explicit dynamic HDYC matrix changes are still rejected");
        }
        if (active->pUnk) active->pUnk->Release();
        CoTaskMemFree(active->pbFormat);
        CoTaskMemFree(active);
    }
    Check(pin.references == 1, "UYVY negotiation releases all COM references");
    selected.present = true;
    selectedSubtype = kMediaSubtypeHdyc;
    Check(ConfigureVideoPin(nullptr, 1920, 1080, 60, VideoPixelFormat::Uyvy,
        bytes, stride, fps, format, nullptr, true, &selected, false, &selectedSubtype) == E_POINTER &&
        !selected.present && selectedSubtype == GUID_NULL,
        "failed negotiation clears both color hints and their subtype provenance");
}

// RGB32 (BI_RGB, 32bpp BGRX) layout and negotiation contract.
void TestRgb32LayoutAndOrientation() {
    using namespace llcv::video;
    Check(PixelFormatFromSubtype(MEDIASUBTYPE_RGB32) == VideoPixelFormat::Rgb32,
        "RGB32 subtype maps to the RGB32 pixel format");
    Check(!IsAutoSelectableVideoFormat(VideoPixelFormat::Rgb32),
        "RGB32 is not an automatic fallback; NV12 auto-priority is unchanged");
    Check(!IsCompressedVideoFormat(VideoPixelFormat::Rgb32), "RGB32 is not compressed");

    const auto makeRgb32 = [](AM_MEDIA_TYPE& media, VIDEOINFOHEADER2& info,
                              int width, LONG biHeight, DWORD sizeImage,
                              DWORD bitCount = 32, DWORD compression = BI_RGB,
                              WORD planes = 1, DWORD headerSize = sizeof(BITMAPINFOHEADER)) {
        info = {};
        info.AvgTimePerFrame = 166'667;
        info.bmiHeader.biSize = headerSize;
        info.bmiHeader.biPlanes = planes;
        info.bmiHeader.biWidth = width;
        info.bmiHeader.biHeight = biHeight;
        info.bmiHeader.biBitCount = static_cast<WORD>(bitCount);
        info.bmiHeader.biCompression = compression;
        info.bmiHeader.biSizeImage = sizeImage;
        media = {};
        media.majortype = MEDIATYPE_Video; media.subtype = MEDIASUBTYPE_RGB32;
        media.formattype = FORMAT_VideoInfo2; media.cbFormat = sizeof(info);
        media.pbFormat = reinterpret_cast<BYTE*>(&info);
    };

    // Odd width/height are fine for packed single-plane RGB, unlike YUV.
    {
        VIDEOINFOHEADER2 info{};
        AM_MEDIA_TYPE media{};
        makeRgb32(media, info, 1921, -1081, 0);
        DWORD bytes = 0; UINT32 stride = 0; int fps = 60;
        Check(SUCCEEDED(ValidateVideoLayout(&media, 1921, 1081, VideoPixelFormat::Rgb32,
            bytes, stride, fps)) && bytes == uint64_t(1921) * 1081 * 4 && stride == 1921 * 4,
            "odd-dimension RGB32 with zero declared size derives exact 4-byte-per-pixel layout");

        info.bmiHeader.biSizeImage = bytes - 1;
        Check(FAILED(ValidateVideoLayout(&media, 1921, 1081, VideoPixelFormat::Rgb32,
            bytes, stride, fps)), "RGB32 payload smaller than width*4*height is rejected");

        info.bmiHeader.biSizeImage = (1921 * 4 + 32) * 1081; // padded stride
        Check(SUCCEEDED(ValidateVideoLayout(&media, 1921, 1081, VideoPixelFormat::Rgb32,
            bytes, stride, fps)) && stride == 1921 * 4 + 32,
            "RGB32 row padding beyond the minimum pitch is preserved exactly");

        // A declared size that is not an exact whole number of rows, or
        // whose resulting pitch is not DWORD-aligned, cannot be a real
        // per-row stride and must not be silently floor-divided into one.
        info.bmiHeader.biSizeImage = (1921 * 4) * 1081 + 1;
        Check(FAILED(ValidateVideoLayout(&media, 1921, 1081, VideoPixelFormat::Rgb32,
            bytes, stride, fps)), "RGB32 size with a non-zero row remainder is rejected");
        info.bmiHeader.biSizeImage = (1921 * 4 + 2) * 1081; // pitch not a multiple of 4
        Check(FAILED(ValidateVideoLayout(&media, 1921, 1081, VideoPixelFormat::Rgb32,
            bytes, stride, fps)), "RGB32 pitch that is not DWORD-aligned is rejected");
    }

    // Only plain BI_RGB 32bpp, or BI_BITFIELDS with the standard BGR masks,
    // is in scope; non-standard masks, truncated mask data, other bit
    // depths, and malformed header size/plane fields are all rejected
    // rather than guessing an unverified channel layout.
    {
        VIDEOINFOHEADER2 info{};
        AM_MEDIA_TYPE media{};
        makeRgb32(media, info, 1920, -1080, 0, 24, BI_RGB);
        DWORD bytes = 0; UINT32 stride = 0; int fps = 60;
        Check(FAILED(ValidateVideoLayout(&media, 1920, 1080, VideoPixelFormat::Rgb32,
            bytes, stride, fps)), "non-32bpp RGB32-labeled layout is rejected");
        makeRgb32(media, info, 1920, -1080, 0, 32, BI_RGB, /*planes*/2);
        Check(FAILED(ValidateVideoLayout(&media, 1920, 1080, VideoPixelFormat::Rgb32,
            bytes, stride, fps)), "biPlanes != 1 is rejected");
        makeRgb32(media, info, 1920, -1080, 0, 32, BI_RGB, /*planes*/1,
            sizeof(BITMAPINFOHEADER) - 1);
        Check(FAILED(ValidateVideoLayout(&media, 1920, 1080, VideoPixelFormat::Rgb32,
            bytes, stride, fps)), "biSize smaller than BITMAPINFOHEADER is rejected");

        // BI_BITFIELDS: the three R/G/B channel masks live immediately after
        // the BITMAPINFOHEADER inside the format block.
        struct BitfieldsLayout { VIDEOINFOHEADER2 header; DWORD masks[3]; };
        BitfieldsLayout layout{};
        layout.header.AvgTimePerFrame = 166'667;
        layout.header.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        layout.header.bmiHeader.biPlanes = 1;
        layout.header.bmiHeader.biWidth = 1920;
        layout.header.bmiHeader.biHeight = -1080;
        layout.header.bmiHeader.biBitCount = 32;
        layout.header.bmiHeader.biCompression = BI_BITFIELDS;
        AM_MEDIA_TYPE bitfieldsMedia{};
        bitfieldsMedia.majortype = MEDIATYPE_Video; bitfieldsMedia.subtype = MEDIASUBTYPE_RGB32;
        bitfieldsMedia.formattype = FORMAT_VideoInfo2; bitfieldsMedia.cbFormat = sizeof(layout);
        bitfieldsMedia.pbFormat = reinterpret_cast<BYTE*>(&layout);
        layout.masks[0] = 0x00FF0000u; layout.masks[1] = 0x0000FF00u; layout.masks[2] = 0x000000FFu;
        Check(SUCCEEDED(ValidateVideoLayout(&bitfieldsMedia, 1920, 1080, VideoPixelFormat::Rgb32,
            bytes, stride, fps)) && stride == 1920 * 4,
            "BI_BITFIELDS with the standard BGR channel masks is accepted");
        layout.masks[0] = 0x000000FFu; layout.masks[2] = 0x00FF0000u; // swapped R/B
        Check(FAILED(ValidateVideoLayout(&bitfieldsMedia, 1920, 1080, VideoPixelFormat::Rgb32,
            bytes, stride, fps)), "BI_BITFIELDS with a non-standard channel order is rejected");
        layout.masks[0] = 0x00FF0000u; layout.masks[2] = 0x000000FFu;
        // sizeof(layout) - 1 is not sufficient: BitfieldsLayout has trailing
        // struct padding (REFERENCE_TIME forces 8-byte alignment) after the
        // masks, so cbFormat must be cut at the mask boundary itself to
        // genuinely truncate the mask data rather than just the padding.
        bitfieldsMedia.cbFormat = static_cast<DWORD>(offsetof(BitfieldsLayout, masks) + 3 * sizeof(DWORD) - 1);
        Check(FAILED(ValidateVideoLayout(&bitfieldsMedia, 1920, 1080, VideoPixelFormat::Rgb32,
            bytes, stride, fps)), "BI_BITFIELDS with truncated mask data is rejected");
    }

    // Orientation: sign of biHeight is preserved end to end, not discarded
    // by an abs()-only height computation the way plain magnitude is.
    {
        VIDEOINFOHEADER2 topDownInfo{};
        AM_MEDIA_TYPE topDownMedia{};
        makeRgb32(topDownMedia, topDownInfo, 4, -4, 64);
        int width = 0, height = 0; REFERENCE_TIME duration = 0; DWORD bytes = 0;
        VideoPixelFormat format = VideoPixelFormat::Auto; bool topDown = false;
        Check(VideoFormatDetails(&topDownMedia, width, height, duration, bytes, &format, &topDown) &&
            height == 4 && topDown, "negative biHeight reports top-down orientation");

        VIDEOINFOHEADER2 bottomUpInfo{};
        AM_MEDIA_TYPE bottomUpMedia{};
        makeRgb32(bottomUpMedia, bottomUpInfo, 4, 4, 64);
        topDown = true;
        Check(VideoFormatDetails(&bottomUpMedia, width, height, duration, bytes, &format, &topDown) &&
            height == 4 && !topDown, "positive biHeight reports bottom-up orientation");

        VideoSampleFormat expected{};
        Check(ReadVideoSampleFormat(&topDownMedia, 60, expected) && expected.topDown,
            "connected RGB32 sample format records top-down orientation");
        Check(!MatchesVideoSampleFormat(&bottomUpMedia, expected),
            "a same-size orientation flip on reconnect is rejected, not silently accepted");

        VideoSampleFormat expectedBottomUp{};
        Check(ReadVideoSampleFormat(&bottomUpMedia, 60, expectedBottomUp) && !expectedBottomUp.topDown,
            "connected RGB32 sample format records bottom-up orientation");
        Check(MatchesVideoSampleFormat(&bottomUpMedia, expectedBottomUp),
            "an unchanged bottom-up RGB32 reannouncement is accepted");
    }

    // Regression: YUV orientation is normalized to top-down regardless of
    // the declared biHeight sign, so a driver flipping the sign alone on
    // reconnect (no real content change) must still compare as unchanged.
    {
        VIDEOINFOHEADER2 info{};
        info.AvgTimePerFrame = 166'667;
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biWidth = 1920; info.bmiHeader.biHeight = -1080;
        AM_MEDIA_TYPE media{};
        media.majortype = MEDIATYPE_Video; media.subtype = MEDIASUBTYPE_NV12;
        media.formattype = FORMAT_VideoInfo2; media.cbFormat = sizeof(info);
        media.pbFormat = reinterpret_cast<BYTE*>(&info);
        VideoSampleFormat expected{};
        Check(ReadVideoSampleFormat(&media, 60, expected) && expected.topDown,
            "NV12 orientation normalizes to top-down for a negative biHeight");
        info.bmiHeader.biHeight = 1080; // sign-only flip, same magnitude
        Check(MatchesVideoSampleFormat(&media, expected),
            "a sign-only NV12 biHeight flip is not treated as a format change");
    }

    // Advertise validated RGB32 capabilities for explicit selection.
    {
        FakeVideoPin pin;
        pin.modes = {{60}};
        pin.modes[0].subtype = MEDIASUBTYPE_RGB32;
        pin.modes[0].imageBytes = 1920 * 1080 * 4;
        pin.modes[0].biBitCount = 32;
        pin.modes[0].biCompression = BI_RGB;
        HRESULT status = S_OK;
        VideoCapabilityDiagnostics diagnostics;
        const auto formats = ProbePixelFormats(&pin, 1920, 1080, &status, nullptr, &diagnostics);
        Check(SUCCEEDED(status) && std::any_of(formats.begin(), formats.end(), [](const auto& item) { return item.format == VideoPixelFormat::Rgb32 && item.selectedFps == 60; }),
            "RGB32 advertised at its supported frame rate");
    }
}

// RGB24 capture contract (BI_RGB only, 24bpp BGR, DWORD-aligned stride).
// Real GC573 hardware advertises this (never RGB32): FORMAT_VideoInfo,
// positive (bottom-up) biHeight, BI_RGB, biPlanes=1, biBitCount=24,
// biSize=sizeof(BITMAPINFOHEADER).
void TestRgb24LayoutAndOrientation() {
    using namespace llcv::video;
    Check(PixelFormatFromSubtype(MEDIASUBTYPE_RGB24) == VideoPixelFormat::Rgb24,
        "RGB24 subtype maps to the RGB24 pixel format");
    Check(!IsAutoSelectableVideoFormat(VideoPixelFormat::Rgb24),
        "RGB24 is not an automatic fallback; NV12 auto-priority is unchanged");
    Check(!IsCompressedVideoFormat(VideoPixelFormat::Rgb24), "RGB24 is not compressed");

    const auto makeRgb24 = [](AM_MEDIA_TYPE& media, VIDEOINFOHEADER& info,
                              int width, LONG biHeight, DWORD sizeImage,
                              DWORD compression = BI_RGB, WORD planes = 1,
                              DWORD headerSize = sizeof(BITMAPINFOHEADER)) {
        info = {};
        info.AvgTimePerFrame = 166'667;
        info.bmiHeader.biSize = headerSize;
        info.bmiHeader.biPlanes = planes;
        info.bmiHeader.biWidth = width;
        info.bmiHeader.biHeight = biHeight;
        info.bmiHeader.biBitCount = 24;
        info.bmiHeader.biCompression = compression;
        info.bmiHeader.biSizeImage = sizeImage;
        media = {};
        media.majortype = MEDIATYPE_Video; media.subtype = MEDIASUBTYPE_RGB24;
        // GC573 advertises RGB24 via the legacy FORMAT_VideoInfo, not
        // FORMAT_VideoInfo2; the parser must handle both for RGB formats.
        media.formattype = FORMAT_VideoInfo; media.cbFormat = sizeof(info);
        media.pbFormat = reinterpret_cast<BYTE*>(&info);
    };

    // Exact real-device fixtures: bottom-up (positive biHeight), stride
    // already DWORD-aligned at these specific widths (no padding needed).
    {
        struct Fixture { int width, height; DWORD stride, sizeImage; };
        for (const auto& fixture : {Fixture{1920, 1080, 5760, 6220800},
                                     Fixture{2560, 1440, 7680, 11059200},
                                     Fixture{3840, 2160, 11520, 24883200}}) {
            VIDEOINFOHEADER info{};
            AM_MEDIA_TYPE media{};
            makeRgb24(media, info, fixture.width, fixture.height, fixture.sizeImage);
            DWORD bytes = 0; UINT32 stride = 0; int fps = 60;
            Check(SUCCEEDED(ValidateVideoLayout(&media, fixture.width, fixture.height,
                VideoPixelFormat::Rgb24, bytes, stride, fps)) &&
                bytes == fixture.sizeImage && stride == fixture.stride,
                "GC573 RGB24 fixture validates with its exact advertised stride/size");
        }
    }

    // DWORD-aligned stride for an arbitrary (non-fixture) odd width: 3
    // bytes/pixel must round up to the next multiple of 4.
    {
        VIDEOINFOHEADER info{};
        AM_MEDIA_TYPE media{};
        makeRgb24(media, info, 1921, 1081, 0); // width*3=5763 -> aligned 5764
        DWORD bytes = 0; UINT32 stride = 0; int fps = 60;
        Check(SUCCEEDED(ValidateVideoLayout(&media, 1921, 1081, VideoPixelFormat::Rgb24,
            bytes, stride, fps)) && stride == 5764 && bytes == uint64_t(5764) * 1081,
            "odd-width RGB24 with zero declared size derives the DWORD-aligned stride");

        info.bmiHeader.biSizeImage = bytes - 1;
        Check(FAILED(ValidateVideoLayout(&media, 1921, 1081, VideoPixelFormat::Rgb24,
            bytes, stride, fps)), "RGB24 payload smaller than the aligned stride*height is rejected");

        info.bmiHeader.biSizeImage = (5764 + 32) * 1081; // extra row padding
        Check(SUCCEEDED(ValidateVideoLayout(&media, 1921, 1081, VideoPixelFormat::Rgb24,
            bytes, stride, fps)) && stride == 5764 + 32,
            "RGB24 row padding beyond the minimum aligned pitch is preserved exactly");

        info.bmiHeader.biSizeImage = (5764) * 1081 + 1; // non-zero row remainder
        Check(FAILED(ValidateVideoLayout(&media, 1921, 1081, VideoPixelFormat::Rgb24,
            bytes, stride, fps)), "RGB24 size with a non-zero row remainder is rejected");

        info.bmiHeader.biSizeImage = (5764 + 2) * 1081; // pitch not DWORD-aligned
        Check(FAILED(ValidateVideoLayout(&media, 1921, 1081, VideoPixelFormat::Rgb24,
            bytes, stride, fps)), "RGB24 pitch that is not DWORD-aligned is rejected");
    }

    // RGB24 has no verified bitfield-mask contract: BI_BITFIELDS is always
    // rejected (unlike RGB32, which accepts the standard BGR masks), along
    // with other bit depths, biPlanes != 1 and a truncated biSize.
    {
        VIDEOINFOHEADER info{};
        AM_MEDIA_TYPE media{};
        DWORD bytes = 0; UINT32 stride = 0; int fps = 60;
        makeRgb24(media, info, 1920, 1080, 0, BI_BITFIELDS);
        Check(FAILED(ValidateVideoLayout(&media, 1920, 1080, VideoPixelFormat::Rgb24,
            bytes, stride, fps)), "BI_BITFIELDS is never accepted for RGB24");
        makeRgb24(media, info, 1920, 1080, 0, BI_RGB, /*planes*/2);
        Check(FAILED(ValidateVideoLayout(&media, 1920, 1080, VideoPixelFormat::Rgb24,
            bytes, stride, fps)), "RGB24 biPlanes != 1 is rejected");
        makeRgb24(media, info, 1920, 1080, 0, BI_RGB, /*planes*/1, sizeof(BITMAPINFOHEADER) - 1);
        Check(FAILED(ValidateVideoLayout(&media, 1920, 1080, VideoPixelFormat::Rgb24,
            bytes, stride, fps)), "RGB24 biSize smaller than BITMAPINFOHEADER is rejected");
    }

    // Orientation: GC573's real convention is bottom-up (positive biHeight);
    // the sign must be preserved end to end and a silent flip rejected.
    {
        VIDEOINFOHEADER bottomUpInfo{};
        AM_MEDIA_TYPE bottomUpMedia{};
        makeRgb24(bottomUpMedia, bottomUpInfo, 4, 4, 0); // width*3=12, already aligned
        int width = 0, height = 0; REFERENCE_TIME duration = 0; DWORD bytes = 0;
        VideoPixelFormat format = VideoPixelFormat::Auto; bool topDown = true;
        Check(VideoFormatDetails(&bottomUpMedia, width, height, duration, bytes, &format, &topDown) &&
            height == 4 && !topDown, "positive biHeight reports bottom-up orientation for RGB24");

        VIDEOINFOHEADER topDownInfo{};
        AM_MEDIA_TYPE topDownMedia{};
        makeRgb24(topDownMedia, topDownInfo, 4, -4, 0);
        topDown = false;
        Check(VideoFormatDetails(&topDownMedia, width, height, duration, bytes, &format, &topDown) &&
            height == 4 && topDown, "negative biHeight reports top-down orientation for RGB24");

        VideoSampleFormat expected{};
        Check(ReadVideoSampleFormat(&bottomUpMedia, 60, expected) && !expected.topDown,
            "connected RGB24 sample format records bottom-up orientation");
        Check(!MatchesVideoSampleFormat(&topDownMedia, expected),
            "a same-size RGB24 orientation flip on reconnect is rejected, not silently accepted");
        Check(MatchesVideoSampleFormat(&bottomUpMedia, expected),
            "an unchanged bottom-up RGB24 reannouncement is accepted");
    }

    // Dynamic type safety: switching between RGB24 and RGB32 at the same
    // resolution must be rejected as a format change, not silently accepted
    // as "still RGB". MatchesVideoSampleFormat compares format directly, so
    // this also exercises that the two enum values never alias each other.
    {
        VIDEOINFOHEADER rgb24Info{};
        AM_MEDIA_TYPE rgb24Media{};
        makeRgb24(rgb24Media, rgb24Info, 4, 4, 0);
        VideoSampleFormat expected{};
        Check(ReadVideoSampleFormat(&rgb24Media, 60, expected), "RGB24 connected signature");

        VIDEOINFOHEADER2 rgb32Info{};
        rgb32Info.AvgTimePerFrame = 166'667;
        rgb32Info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        rgb32Info.bmiHeader.biPlanes = 1;
        rgb32Info.bmiHeader.biWidth = 4; rgb32Info.bmiHeader.biHeight = 4;
        rgb32Info.bmiHeader.biBitCount = 32; rgb32Info.bmiHeader.biCompression = BI_RGB;
        AM_MEDIA_TYPE rgb32Media{};
        rgb32Media.majortype = MEDIATYPE_Video; rgb32Media.subtype = MEDIASUBTYPE_RGB32;
        rgb32Media.formattype = FORMAT_VideoInfo2; rgb32Media.cbFormat = sizeof(rgb32Info);
        rgb32Media.pbFormat = reinterpret_cast<BYTE*>(&rgb32Info);
        Check(!MatchesVideoSampleFormat(&rgb32Media, expected),
            "a silent RGB24 -> RGB32 subtype switch at the same size is rejected");
    }

    // Advertise validated RGB24 capabilities for explicit selection.
    {
        FakeVideoPin pin;
        pin.modes = {{60}};
        pin.modes[0].subtype = MEDIASUBTYPE_RGB24;
        pin.modes[0].imageBytes = 5760 * 1080;
        pin.modes[0].biBitCount = 24;
        pin.modes[0].biCompression = BI_RGB;
        HRESULT status = S_OK;
        VideoCapabilityDiagnostics diagnostics;
        const auto formats = ProbePixelFormats(&pin, 1920, 1080, &status, nullptr, &diagnostics);
        Check(SUCCEEDED(status) && std::any_of(formats.begin(), formats.end(), [](const auto& item) { return item.format == VideoPixelFormat::Rgb24 && item.selectedFps == 60; }),
            "RGB24 advertised at its supported frame rate");
    }
}

void TestRgbRejectionDiagnostics() {
    using namespace llcv::video;
    struct Layout { VIDEOINFOHEADER2 info; DWORD masks[3]; } layout{};
    AM_MEDIA_TYPE media{};
    media.majortype = MEDIATYPE_Video; media.formattype = FORMAT_VideoInfo2;
    media.pbFormat = reinterpret_cast<BYTE*>(&layout);
    for (const auto format : {VideoPixelFormat::Rgb24, VideoPixelFormat::Rgb32}) {
        const auto reset = [&] {
            layout = {}; media.cbFormat = sizeof(layout);
            media.subtype = format == VideoPixelFormat::Rgb24 ? MEDIASUBTYPE_RGB24 : MEDIASUBTYPE_RGB32;
            layout.info.AvgTimePerFrame = 166667;
            auto& b = layout.info.bmiHeader;
            b.biSize = sizeof(BITMAPINFOHEADER); b.biWidth = 4; b.biHeight = -4;
            b.biBitCount = format == VideoPixelFormat::Rgb24 ? 24 : 32; b.biPlanes = 1;
        };
        const auto reject = [&](const wchar_t* reason) {
            DWORD bytes = 123; UINT32 stride = 456; int fps = 60;
            selectionLog.clear();
            Check(FAILED(ValidateVideoLayout(&media, 4, 4, format, bytes, stride, fps, SelectionLog)) &&
                bytes == 123 && stride == 456 && fps == 60, "rejection leaves caller outputs unchanged");
            Check(selectionLog.find(std::wstring(L"reason=") + reason) != std::wstring::npos &&
                selectionLog.find(L"formatBytes=") != std::wstring::npos &&
                selectionLog.find(L"biSize=") != std::wstring::npos &&
                selectionLog.find(L"bpp=") != std::wstring::npos &&
                selectionLog.find(L"compression=") != std::wstring::npos &&
                selectionLog.find(L"imageBytes=") != std::wstring::npos,
                "invalid RGB layout includes specific reason and bounded header details");
        };
        for (DWORD size : {DWORD(sizeof(BITMAPV4HEADER)), DWORD(sizeof(BITMAPV5HEADER)), 0xfffffffful}) {
            reset(); layout.info.bmiHeader.biSize = size; reject(L"rgb-header-size");
        }
        reset(); layout.info.bmiHeader.biBitCount = 16; reject(L"rgb-bit-depth");
        reset(); layout.info.bmiHeader.biPlanes = 2; reject(L"rgb-plane-count");
        reset(); layout.info.bmiHeader.biCompression = BI_RLE8; reject(L"rgb-compression");
        reset(); layout.info.bmiHeader.biSizeImage = 1; reject(L"rgb-image-size");
        reset(); layout.info.bmiHeader.biSizeImage = 1001; reject(L"rgb-row-remainder");
        reset(); layout.info.bmiHeader.biSizeImage = 72; reject(L"rgb-row-alignment");
        reset(); layout.info.bmiHeader.biCompression = BI_BITFIELDS;
        if (format == VideoPixelFormat::Rgb32) {
            layout.masks[0] = 0xff; layout.masks[1] = 0xff00; layout.masks[2] = 0xff0000;
            reject(L"rgb-channel-masks");
            Check(selectionLog.find(L"masks=present:000000FF/0000FF00/00FF0000") != std::wstring::npos,
                "nonstandard RGB masks are logged verbatim");
            media.cbFormat = sizeof(VIDEOINFOHEADER2) + 11; reject(L"rgb-masks-truncated");
            Check(selectionLog.find(L"masks=unavailable") != std::wstring::npos,
                "truncated masks are never read for logging");
        } else reject(L"rgb-compression");
        reset(); media.cbFormat = 1; reject(L"geometry-format-or-timing");
    }
    FakeVideoPin pin; pin.modes = {{60}};
    pin.modes[0].subtype = MEDIASUBTYPE_RGB32; pin.modes[0].biBitCount = 24;
    selectionLog.clear(); HRESULT status = S_OK;
    Check(ProbePixelFormats(&pin, 1920, 1080, &status, SelectionLog).empty() && FAILED(status) &&
        selectionLog.find(L"reason=rgb-bit-depth") != std::wstring::npos,
        "capability probing forwards detailed RGB rejection diagnostics");
}

}  // namespace

#include "VideoFaultInjection.inl"

int main(int argc, char** argv) {
    if (argc == 2 && std::string(argv[1]) == "--faults") return RunVideoFaultInjection();
    if (argc == 2 && std::string(argv[1]) == "--stress") return RunVideoLayoutStress();
    TestPixelFormatHelpers();
    TestVideoInfo2Details();
    TestColorMetadataMerge();
    TestChangingDriverCapabilities();
    TestSelectedVersusActiveFormat();
    TestExplicitFrameRates();
    TestStrictListedFrameRates();
    TestAcceptedCandidateColorMetadata();
    TestLayoutBoundariesAndProbeErrors();
    TestMalformedCapabilityOwnership();
    TestUnsupportedFormatDiagnostics();
    TestUyvyNegotiationAndColor();
    TestRgb32LayoutAndOrientation();
    TestRgb24LayoutAndOrientation();
    TestRgbRejectionDiagnostics();
    if (failures != 0) {
        std::fprintf(stderr, "%d test(s) failed.\n", failures);
        return 1;
    }
    std::puts("DirectShow video format tests passed.");
    return 0;
}
