#include "video/DirectShowVideoFormat.h"

#include <dvdmedia.h>
#include <mfapi.h>

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
    Check(llcv::video::PixelFormatFromSubtype(MEDIASUBTYPE_NV12) ==
              VideoPixelFormat::Nv12,
          "NV12 subtype");
    Check(llcv::video::PixelFormatFromSubtype(MEDIASUBTYPE_YUY2) ==
              VideoPixelFormat::Yuy2,
          "YUY2 subtype");
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
    TestLayoutBoundariesAndProbeErrors();
    if (failures != 0) {
        std::fprintf(stderr, "%d test(s) failed.\n", failures);
        return 1;
    }
    std::puts("DirectShow video format tests passed.");
    return 0;
}
