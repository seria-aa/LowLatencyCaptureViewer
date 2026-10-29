#pragma once

#include "settings/AppSettings.h"
#include "video/CaptureColorMetadata.h"

#include <dshow.h>

#include <string>
#include <vector>

namespace llcv::video {

using VideoPixelFormat = settings::VideoPixelFormat;
using LogCallback = void (*)(const wchar_t* message);

// HDYC uses UYVY byte ordering with BT.709 color components. Not declared by
// every Windows SDK; FourCC 'HDYC' in the standard media-subtype GUID family.
inline constexpr GUID kMediaSubtypeHdyc{
    0x43594448, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

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

// topDown reports the DIB row direction, but only RGB24/RGB32 genuinely vary: their
// biHeight sign is taken as-is. YUV/MJPEG orientation is normalized to true
// regardless of the declared sign, since DirectShow YUV capture is top-down
// in practice and some drivers do not encode the sign meaningfully for it;
// this keeps YUV dynamic-type comparisons unaffected. Callers that do not
// consume orientation can omit it.
bool VideoFormatDetails(const AM_MEDIA_TYPE* mediaType, int& width,
                        int& height, REFERENCE_TIME& frameDuration,
                        DWORD& imageBytes,
                        VideoPixelFormat* pixelFormat = nullptr,
                        bool* topDown = nullptr);
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
    int wantedHeight, int wantedFps, CaptureColorMetadata& metadata,
    const GUID* wantedSubtype = nullptr);

// Optional color hints are cleared on entry and taken only from the accepted
// candidate. Active/connected metadata must override these startup-only hints.
// The optional subtype identifies their source if a later driver readback
// changes between UYVY and HDYC, which have different color defaults.
// Explicit frame selections require strict verification. Requesting a timing
// absent from the advertised modes/ranges is a separate, explicit opt-in.
HRESULT ConfigureVideoPin(
    IPin* videoPin, int wantedWidth, int wantedHeight, int wantedFps,
    VideoPixelFormat wantedFormat, DWORD& imageBytes, UINT32& stride,
    int& configuredFps, VideoPixelFormat& configuredFormat,
    LogCallback logCallback, bool requestExactFps = false,
    CaptureColorMetadata* acceptedCandidateColor = nullptr,
    bool allowUnlistedFps = false,
    GUID* acceptedCandidateSubtype = nullptr);
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
// Optional failure diagnostics include a stable reason and bounded header details.
HRESULT ValidateVideoLayout(const AM_MEDIA_TYPE* mediaType,
    int expectedWidth, int expectedHeight, VideoPixelFormat expectedFormat,
    DWORD& imageBytes, UINT32& stride, int& fps,
    LogCallback logCallback = nullptr);

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
    // DIB row direction at connection time. Default matches the top-down
    // convention assumed everywhere before RGB support existed; YUV capture
    // is effectively always top-down in practice so this preserves prior
    // behavior for those formats. See VideoFormatDetails for the source bit.
    bool topDown = true;
};
bool ReadVideoSampleFormat(const AM_MEDIA_TYPE* mediaType, int fallbackFps,
                           VideoSampleFormat& result);
bool MatchesVideoSampleFormat(const AM_MEDIA_TYPE* mediaType,
                              const VideoSampleFormat& expected);

}  // namespace llcv::video
