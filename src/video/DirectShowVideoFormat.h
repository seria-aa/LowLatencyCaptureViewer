#pragma once

#include "settings/AppSettings.h"
#include "video/CaptureColorMetadata.h"

#include <dshow.h>

#include <string>
#include <vector>

namespace llcv::video {

using VideoPixelFormat = settings::VideoPixelFormat;
using LogCallback = void (*)(const wchar_t* message);

struct PixelFormatSupport {
    VideoPixelFormat format = VideoPixelFormat::Auto;
    int selectedFps = 0;
};

// Startup/settings diagnostics, separate from query HRESULTs. Only media types
// at the requested resolution contribute: an unsupported format elsewhere must
// not hide a simple resolution mismatch.
struct VideoCapabilityDiagnostics {
    unsigned matchingResolutionTypes = 0;
    unsigned unsupportedMatchingTypes = 0;
    std::vector<std::wstring> unsupportedFormatNames;

    bool UnsupportedFormatsOnly() const {
        return matchingResolutionTypes != 0 &&
            matchingResolutionTypes == unsupportedMatchingTypes;
    }
};

const wchar_t* PixelFormatName(VideoPixelFormat format);
bool IsCompressedVideoFormat(VideoPixelFormat format);
bool IsAutoSelectableVideoFormat(VideoPixelFormat format);
VideoPixelFormat PixelFormatFromSubtype(const GUID& subtype);

bool VideoFormatDetails(const AM_MEDIA_TYPE* mediaType, int& width,
                        int& height, REFERENCE_TIME& frameDuration,
                        DWORD& imageBytes,
                        VideoPixelFormat* pixelFormat = nullptr);
bool ExtractDirectShowColorMetadata(
    const AM_MEDIA_TYPE* mediaType, CaptureColorMetadata& metadata);
void MergeDirectShowColorMetadata(
    CaptureColorMetadata& destination,
    const CaptureColorMetadata& overrideValues);
void LogDirectShowColorMetadata(
    const wchar_t* source, const CaptureColorMetadata& metadata,
    LogCallback logCallback);
bool FindMatchingDirectShowColorMetadata(
    IPin* videoPin, VideoPixelFormat wantedFormat, int wantedWidth,
    int wantedHeight, int wantedFps, CaptureColorMetadata& metadata);

// Optional color hints are cleared on entry and taken only from the accepted
// candidate. Active/connected metadata must override these startup-only hints.
// Explicit frame selections require strict verification. Requesting a timing
// absent from the advertised modes/ranges is a separate, explicit opt-in.
HRESULT ConfigureVideoPin(
    IPin* videoPin, int wantedWidth, int wantedHeight, int wantedFps,
    VideoPixelFormat wantedFormat, DWORD& imageBytes, UINT32& stride,
    int& configuredFps, VideoPixelFormat& configuredFormat,
    LogCallback logCallback, bool requestExactFps = false,
    CaptureColorMetadata* acceptedCandidateColor = nullptr,
    bool allowUnlistedFps = false);
// Strict timing check used at negotiation and after graph connection.
bool IsRequestedFrameRate(const AM_MEDIA_TYPE* mediaType, int fps);
inline constexpr HRESULT kRequestedFrameRateMismatch =
    MAKE_HRESULT(SEVERITY_ERROR, FACILITY_ITF, 0x301);
HRESULT GetActiveVideoPinFormat(IPin* videoPin, AM_MEDIA_TYPE** mediaType);
std::vector<PixelFormatSupport> ProbePixelFormats(
    IPin* videoPin, int width, int height, HRESULT* queryStatus = nullptr,
    LogCallback logCallback = nullptr,
    VideoCapabilityDiagnostics* diagnostics = nullptr);

// Startup-only validation. Outputs change only after a complete valid layout.
// fps supplies the negotiated fallback when the media type omits frame timing.
HRESULT ValidateVideoLayout(const AM_MEDIA_TYPE* mediaType,
    int expectedWidth, int expectedHeight, VideoPixelFormat expectedFormat,
    DWORD& imageBytes, UINT32& stride, int& fps);

// Immutable, normalized description of the connected sample format. Compared
// only when a sample actually carries a dynamic media type, never per pixel.
struct VideoSampleFormat {
    bool valid = false;
    VideoPixelFormat format = VideoPixelFormat::Auto;
    int width = 0, height = 0, fps = 0;
    DWORD bytes = 0, interlace = 0;
    UINT32 stride = 0;
    RECT source{}, target{};
    DWORD aspectX = 0, aspectY = 0;
    CaptureColorMetadata color{};
};
bool ReadVideoSampleFormat(const AM_MEDIA_TYPE* mediaType, int fallbackFps,
                           VideoSampleFormat& result);
bool MatchesVideoSampleFormat(const AM_MEDIA_TYPE* mediaType,
                              const VideoSampleFormat& expected);

}  // namespace llcv::video
