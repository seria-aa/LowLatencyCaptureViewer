#pragma once

#include "settings/AppSettings.h"
#include "video/CaptureColorMetadata.h"

#include <dshow.h>

#include <vector>

namespace llcv::video {

using VideoPixelFormat = settings::VideoPixelFormat;
using LogCallback = void (*)(const wchar_t* message);

struct PixelFormatSupport {
    VideoPixelFormat format = VideoPixelFormat::Auto;
    int selectedFps = 0;
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

HRESULT ConfigureVideoPin(
    IPin* videoPin, int wantedWidth, int wantedHeight, int wantedFps,
    VideoPixelFormat wantedFormat, DWORD& imageBytes, UINT32& stride,
    int& configuredFps, VideoPixelFormat& configuredFormat,
    LogCallback logCallback);
HRESULT GetActiveVideoPinFormat(IPin* videoPin, AM_MEDIA_TYPE** mediaType);
std::vector<PixelFormatSupport> ProbePixelFormats(
    IPin* videoPin, int width, int height);

}  // namespace llcv::video
