#include "video/DirectShowVideoFormat.h"

#include <dvdmedia.h>
#include <dxva.h>
#include <mfapi.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace llcv::video {
namespace {

template<class T>
void SafeRelease(T*& value) {
    if (value) {
        value->Release();
        value = nullptr;
    }
}

void FreeMediaType(AM_MEDIA_TYPE& mediaType) {
    if (mediaType.cbFormat != 0) {
        CoTaskMemFree(mediaType.pbFormat);
        mediaType.cbFormat = 0;
        mediaType.pbFormat = nullptr;
    }
    if (mediaType.pUnk) {
        mediaType.pUnk->Release();
        mediaType.pUnk = nullptr;
    }
}

void DeleteMediaType(AM_MEDIA_TYPE* mediaType) {
    if (!mediaType) return;
    FreeMediaType(*mediaType);
    CoTaskMemFree(mediaType);
}

bool SetVideoFrameDuration(AM_MEDIA_TYPE* mediaType,
                           REFERENCE_TIME frameDuration) {
    if (!mediaType || frameDuration <= 0) return false;
    if (mediaType->formattype == FORMAT_VideoInfo2 &&
        mediaType->cbFormat >= sizeof(VIDEOINFOHEADER2)) {
        auto* info = reinterpret_cast<VIDEOINFOHEADER2*>(mediaType->pbFormat);
        info->AvgTimePerFrame = frameDuration;
        return true;
    }
    if (mediaType->formattype == FORMAT_VideoInfo &&
        mediaType->cbFormat >= sizeof(VIDEOINFOHEADER)) {
        auto* info = reinterpret_cast<VIDEOINFOHEADER*>(mediaType->pbFormat);
        info->AvgTimePerFrame = frameDuration;
        return true;
    }
    if (mediaType->formattype == FORMAT_MPEG2Video &&
        mediaType->cbFormat >= FIELD_OFFSET(MPEG2VIDEOINFO, dwSequenceHeader)) {
        auto* info = reinterpret_cast<MPEG2VIDEOINFO*>(mediaType->pbFormat);
        info->hdr.AvgTimePerFrame = frameDuration;
        return true;
    }
    return false;
}

REFERENCE_TIME FrameDurationForRate(int fps) {
    return fps > 0 ? (10'000'000 + fps / 2) / fps : 0;
}

bool FrameRateAllowedByCaps(const VIDEO_STREAM_CONFIG_CAPS& caps, int fps) {
    const REFERENCE_TIME duration = FrameDurationForRate(fps);
    if (duration <= 0 || caps.MinFrameInterval <= 0 ||
        caps.MaxFrameInterval <= 0) {
        return false;
    }
    const REFERENCE_TIME minimum =
        std::min(caps.MinFrameInterval, caps.MaxFrameInterval);
    const REFERENCE_TIME maximum =
        std::max(caps.MinFrameInterval, caps.MaxFrameInterval);
    return duration >= minimum && duration <= maximum;
}

const wchar_t* DirectShowTransferName(UINT value) {
    switch (value) {
    case 5: return L"BT.709";
    case 12: return L"BT.2020 constant";
    case 13: return L"BT.2020";
    case 15: return L"PQ/ST.2084";
    case 16: return L"HLG";
    default: return value == 0 ? L"unknown" : L"unrecognized";
    }
}

const wchar_t* DirectShowPrimariesName(UINT value) {
    switch (value) {
    case 2: return L"BT.709";
    case 9: return L"BT.2020";
    case 11: return L"DCI-P3";
    default: return value == 0 ? L"unknown" : L"unrecognized";
    }
}

int PixelFormatRank(VideoPixelFormat format) {
    switch (format) {
    case VideoPixelFormat::Nv12: return 0;
    case VideoPixelFormat::Yuy2: return 1;
    case VideoPixelFormat::P010: return 2;
    case VideoPixelFormat::Mjpeg: return 3;
    default: return 3;
    }
}

}  // namespace

const wchar_t* PixelFormatName(VideoPixelFormat format) {
    switch (format) {
    case VideoPixelFormat::Mjpeg: return L"MJPEG";
    case VideoPixelFormat::Yuy2: return L"YUY2";
    case VideoPixelFormat::Nv12: return L"NV12";
    case VideoPixelFormat::P010: return L"P010";
    default: return L"Auto";
    }
}

bool IsCompressedVideoFormat(VideoPixelFormat format) {
    return format == VideoPixelFormat::Mjpeg;
}

bool IsAutoSelectableVideoFormat(VideoPixelFormat format) {
    return format == VideoPixelFormat::Nv12 ||
           format == VideoPixelFormat::Yuy2;
}

VideoPixelFormat PixelFormatFromSubtype(const GUID& subtype) {
    if (subtype == MEDIASUBTYPE_NV12) return VideoPixelFormat::Nv12;
    if (subtype == MEDIASUBTYPE_YUY2) return VideoPixelFormat::Yuy2;
    if (subtype == MEDIASUBTYPE_MJPG || subtype == MFVideoFormat_MJPG) {
        return VideoPixelFormat::Mjpeg;
    }
    if (subtype == MFVideoFormat_P010) return VideoPixelFormat::P010;
    return VideoPixelFormat::Auto;
}

bool VideoFormatDetails(const AM_MEDIA_TYPE* mediaType, int& width,
                        int& height, REFERENCE_TIME& frameDuration,
                        DWORD& imageBytes, VideoPixelFormat* pixelFormat) {
    if (!mediaType || mediaType->majortype != MEDIATYPE_Video) return false;
    const VideoPixelFormat detected = PixelFormatFromSubtype(mediaType->subtype);
    if (detected == VideoPixelFormat::Auto) return false;
    if (pixelFormat) *pixelFormat = detected;

    const BITMAPINFOHEADER* bitmap = nullptr;
    frameDuration = 0;
    if (mediaType->formattype == FORMAT_VideoInfo2 &&
        mediaType->cbFormat >= sizeof(VIDEOINFOHEADER2)) {
        const auto* info =
            reinterpret_cast<const VIDEOINFOHEADER2*>(mediaType->pbFormat);
        bitmap = &info->bmiHeader;
        frameDuration = info->AvgTimePerFrame;
    } else if (mediaType->formattype == FORMAT_VideoInfo &&
               mediaType->cbFormat >= sizeof(VIDEOINFOHEADER)) {
        const auto* info =
            reinterpret_cast<const VIDEOINFOHEADER*>(mediaType->pbFormat);
        bitmap = &info->bmiHeader;
        frameDuration = info->AvgTimePerFrame;
    } else if (mediaType->formattype == FORMAT_MPEG2Video &&
               mediaType->cbFormat >=
                   FIELD_OFFSET(MPEG2VIDEOINFO, dwSequenceHeader)) {
        const auto* info =
            reinterpret_cast<const MPEG2VIDEOINFO*>(mediaType->pbFormat);
        bitmap = &info->hdr.bmiHeader;
        frameDuration = info->hdr.AvgTimePerFrame;
    }
    if (!bitmap) return false;
    width = static_cast<int>(bitmap->biWidth);
    height = std::abs(static_cast<int>(bitmap->biHeight));
    imageBytes = bitmap->biSizeImage;
    return width > 0 && height > 0;
}

bool ExtractDirectShowColorMetadata(
    const AM_MEDIA_TYPE* mediaType, CaptureColorMetadata& metadata) {
    metadata = {};
    if (!mediaType || !mediaType->pbFormat) return false;

    const VIDEOINFOHEADER2* info = nullptr;
    if (mediaType->formattype == FORMAT_VideoInfo2 &&
        mediaType->cbFormat >= sizeof(VIDEOINFOHEADER2)) {
        info = reinterpret_cast<const VIDEOINFOHEADER2*>(mediaType->pbFormat);
    } else if (mediaType->formattype == FORMAT_MPEG2Video &&
               mediaType->cbFormat >= sizeof(MPEG2VIDEOINFO)) {
        info = &reinterpret_cast<const MPEG2VIDEOINFO*>(mediaType->pbFormat)->hdr;
    }
    if (!info || (info->dwControlFlags & AMCONTROL_COLORINFO_PRESENT) == 0) {
        return false;
    }

    const DWORD flags = info->dwControlFlags;
    metadata.present = true;
    metadata.controlFlags = flags;
    metadata.chromaSubsampling = DXVA_ExtractExtColorData(
        flags, DXVA_VideoChromaSubsamplingMask,
        DXVA_VideoChromaSubsamplingShift);
    metadata.nominalRange = DXVA_ExtractExtColorData(
        flags, DXVA_NominalRangeMask, DXVA_NominalRangeShift);
    metadata.transferMatrix = DXVA_ExtractExtColorData(
        flags, DXVA_VideoTransferMatrixMask, DXVA_VideoTransferMatrixShift);
    metadata.lighting = DXVA_ExtractExtColorData(
        flags, DXVA_VideoLightingMask, DXVA_VideoLightingShift);
    metadata.primaries = DXVA_ExtractExtColorData(
        flags, DXVA_VideoPrimariesMask, DXVA_VideoPrimariesShift);
    metadata.transferFunction = DXVA_ExtractExtColorData(
        flags, DXVA_VideoTransFuncMask, DXVA_VideoTransFuncShift);
    return true;
}

void MergeDirectShowColorMetadata(
    CaptureColorMetadata& destination,
    const CaptureColorMetadata& overrideValues) {
    if (!overrideValues.present) return;
    destination.present = true;
    destination.controlFlags = overrideValues.controlFlags;
    if (overrideValues.chromaSubsampling != 0) {
        destination.chromaSubsampling = overrideValues.chromaSubsampling;
    }
    if (overrideValues.nominalRange != 0) {
        destination.nominalRange = overrideValues.nominalRange;
    }
    if (overrideValues.transferMatrix != 0) {
        destination.transferMatrix = overrideValues.transferMatrix;
    }
    if (overrideValues.lighting != 0) {
        destination.lighting = overrideValues.lighting;
    }
    if (overrideValues.primaries != 0) {
        destination.primaries = overrideValues.primaries;
    }
    if (overrideValues.transferFunction != 0) {
        destination.transferFunction = overrideValues.transferFunction;
    }
}

void LogDirectShowColorMetadata(
    const wchar_t* source, const CaptureColorMetadata& metadata,
    LogCallback logCallback) {
    if (!logCallback) return;
    wchar_t message[512]{};
    if (!metadata.present) {
        swprintf_s(
            message,
            L"[video-color] DirectShow color info (%s): unavailable.\n",
            source ? source : L"unknown");
        logCallback(message);
        return;
    }
    swprintf_s(
        message,
        L"[video-color] DirectShow color info (%s): flags=0x%08X, "
        L"primaries=%u (%s), transfer=%u (%s), matrix=%u, range=%u%s.\n",
        source ? source : L"unknown",
        static_cast<unsigned>(metadata.controlFlags), metadata.primaries,
        DirectShowPrimariesName(metadata.primaries), metadata.transferFunction,
        DirectShowTransferName(metadata.transferFunction),
        metadata.transferMatrix, metadata.nominalRange,
        metadata.hdr10() ? L" [HDR10 candidate]" : L"");
    logCallback(message);
}

bool FindMatchingDirectShowColorMetadata(
    IPin* videoPin, VideoPixelFormat wantedFormat, int wantedWidth,
    int wantedHeight, int wantedFps, CaptureColorMetadata& metadata) {
    metadata = {};
    if (!videoPin) return false;
    IAMStreamConfig* config = nullptr;
    HRESULT hr = videoPin->QueryInterface(IID_PPV_ARGS(&config));
    if (FAILED(hr)) return false;

    int count = 0;
    int capBytes = 0;
    hr = config->GetNumberOfCapabilities(&count, &capBytes);
    if (FAILED(hr) ||
        capBytes < static_cast<int>(sizeof(VIDEO_STREAM_CONFIG_CAPS))) {
        SafeRelease(config);
        return false;
    }
    std::vector<BYTE> caps(static_cast<size_t>(capBytes));
    bool found = false;
    for (int i = 0; i < count && !found; ++i) {
        AM_MEDIA_TYPE* candidate = nullptr;
        if (FAILED(config->GetStreamCaps(i, &candidate, caps.data())) ||
            !candidate) {
            continue;
        }
        int width = 0;
        int height = 0;
        REFERENCE_TIME duration = 0;
        DWORD bytes = 0;
        VideoPixelFormat format = VideoPixelFormat::Auto;
        const bool details = VideoFormatDetails(
            candidate, width, height, duration, bytes, &format);
        const int fps = duration > 0
            ? static_cast<int>((10'000'000 + duration / 2) / duration) : 0;
        CaptureColorMetadata candidateMetadata{};
        const bool hasColor =
            ExtractDirectShowColorMetadata(candidate, candidateMetadata);
        if (details && format == wantedFormat && width == wantedWidth &&
            height == wantedHeight && fps == wantedFps && hasColor) {
            metadata = candidateMetadata;
            found = true;
        }
        DeleteMediaType(candidate);
    }
    SafeRelease(config);
    return found;
}

HRESULT ConfigureVideoPin(
    IPin* videoPin, int wantedWidth, int wantedHeight, int wantedFps,
    VideoPixelFormat wantedFormat, DWORD& imageBytes, UINT32& stride,
    int& configuredFps, VideoPixelFormat& configuredFormat,
    LogCallback logCallback) {
    IAMStreamConfig* config = nullptr;
    HRESULT hr = videoPin->QueryInterface(IID_PPV_ARGS(&config));
    if (FAILED(hr)) return hr;

    int count = 0;
    int capBytes = 0;
    hr = config->GetNumberOfCapabilities(&count, &capBytes);
    if (FAILED(hr) ||
        capBytes < static_cast<int>(sizeof(VIDEO_STREAM_CONFIG_CAPS))) {
        SafeRelease(config);
        return FAILED(hr) ? hr : E_FAIL;
    }

    struct FormatCandidate {
        AM_MEDIA_TYPE* mediaType = nullptr;
        int fps = 0;
        DWORD imageBytes = 0;
        VideoPixelFormat format = VideoPixelFormat::Nv12;
    };
    std::vector<BYTE> caps(static_cast<size_t>(capBytes));
    std::vector<FormatCandidate> candidates;
    for (int i = 0; i < count; ++i) {
        AM_MEDIA_TYPE* candidate = nullptr;
        hr = config->GetStreamCaps(i, &candidate, caps.data());
        if (FAILED(hr) || !candidate) continue;

        VIDEO_STREAM_CONFIG_CAPS streamCaps{};
        std::memcpy(&streamCaps, caps.data(), sizeof(streamCaps));
        int width = 0;
        int height = 0;
        REFERENCE_TIME duration = 0;
        DWORD bytes = 0;
        VideoPixelFormat format = VideoPixelFormat::Nv12;
        const bool details = VideoFormatDetails(
            candidate, width, height, duration, bytes, &format);
        int fps = duration > 0
            ? static_cast<int>((10'000'000 + duration / 2) / duration) : 0;
        const bool formatMatches = wantedFormat == VideoPixelFormat::Auto
            ? IsAutoSelectableVideoFormat(format) : format == wantedFormat;
        if (details && formatMatches && width == wantedWidth &&
            height == wantedHeight && fps > 0) {
            if (wantedFps == 30 && fps != wantedFps &&
                FrameRateAllowedByCaps(streamCaps, wantedFps) &&
                SetVideoFrameDuration(
                    candidate, FrameDurationForRate(wantedFps))) {
                fps = wantedFps;
            }
            candidates.push_back({candidate, fps, bytes, format});
            continue;
        }
        DeleteMediaType(candidate);
    }

    if (candidates.empty()) {
        SafeRelease(config);
        return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
    }

    std::stable_sort(
        candidates.begin(), candidates.end(),
        [wantedFps, wantedFormat](const FormatCandidate& a,
                                 const FormatCandidate& b) {
            if (wantedFormat == VideoPixelFormat::Auto &&
                a.format != b.format) {
                return PixelFormatRank(a.format) < PixelFormatRank(b.format);
            }
            if (a.fps == wantedFps || b.fps == wantedFps) {
                return a.fps == wantedFps && b.fps != wantedFps;
            }
            const bool aWithin = a.fps < wantedFps;
            const bool bWithin = b.fps < wantedFps;
            if (aWithin != bWithin) return aWithin;
            return aWithin ? a.fps > b.fps : a.fps < b.fps;
        });

    hr = HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
    FormatCandidate* selected = nullptr;
    for (auto& candidate : candidates) {
        hr = config->SetFormat(candidate.mediaType);
        if (SUCCEEDED(hr)) {
            selected = &candidate;
            break;
        }
    }

    if (selected) {
        imageBytes = selected->imageBytes;
        configuredFps = selected->fps;
        configuredFormat = selected->format;
        if (!imageBytes && !IsCompressedVideoFormat(configuredFormat)) {
            imageBytes = static_cast<DWORD>(
                configuredFormat == VideoPixelFormat::Yuy2
                    ? wantedWidth * wantedHeight * 2
                    : configuredFormat == VideoPixelFormat::P010
                        ? wantedWidth * wantedHeight * 3
                        : wantedWidth * wantedHeight * 3 / 2);
        }
        const uint64_t derivedStride =
            configuredFormat == VideoPixelFormat::Yuy2
                ? static_cast<uint64_t>(imageBytes) / wantedHeight
                : configuredFormat == VideoPixelFormat::P010
                    ? static_cast<uint64_t>(imageBytes) * 2 /
                          (static_cast<uint64_t>(wantedHeight) * 3)
                    : (static_cast<uint64_t>(imageBytes) * 2) /
                          (static_cast<uint64_t>(wantedHeight) * 3);
        const UINT32 minimumStride =
            configuredFormat == VideoPixelFormat::Yuy2
                ? static_cast<UINT32>(wantedWidth * 2)
                : configuredFormat == VideoPixelFormat::P010
                    ? static_cast<UINT32>(wantedWidth * 2)
                    : static_cast<UINT32>(wantedWidth);
        stride = IsCompressedVideoFormat(configuredFormat)
            ? 0
            : static_cast<UINT32>(derivedStride >= minimumStride
                                      ? derivedStride : minimumStride);
        if (configuredFps != wantedFps && logCallback) {
            wchar_t message[320]{};
            swprintf_s(
                message,
                L"[video] requested %s %dx%d @ %d unavailable; "
                L"using %d fps at the exact resolution.\n",
                PixelFormatName(configuredFormat), wantedWidth,
                wantedHeight, wantedFps, configuredFps);
            logCallback(message);
        }
    }
    for (auto& candidate : candidates) DeleteMediaType(candidate.mediaType);
    SafeRelease(config);
    return hr;
}

HRESULT GetActiveVideoPinFormat(IPin* videoPin, AM_MEDIA_TYPE** mediaType) {
    if (!videoPin || !mediaType) return E_POINTER;
    *mediaType = nullptr;
    IAMStreamConfig* config = nullptr;
    HRESULT hr = videoPin->QueryInterface(IID_PPV_ARGS(&config));
    if (SUCCEEDED(hr)) hr = config->GetFormat(mediaType);
    SafeRelease(config);
    return hr;
}

std::vector<PixelFormatSupport> ProbePixelFormats(
    IPin* videoPin, int width, int height) {
    std::vector<PixelFormatSupport> result;
    if (!videoPin) return result;
    IAMStreamConfig* config = nullptr;
    HRESULT hr = videoPin->QueryInterface(IID_PPV_ARGS(&config));
    int count = 0;
    int capBytes = 0;
    if (SUCCEEDED(hr)) hr = config->GetNumberOfCapabilities(&count, &capBytes);
    std::vector<BYTE> caps(capBytes > 0 ? static_cast<size_t>(capBytes) : 1);
    for (int i = 0; SUCCEEDED(hr) && i < count; ++i) {
        AM_MEDIA_TYPE* mediaType = nullptr;
        if (FAILED(config->GetStreamCaps(i, &mediaType, caps.data())) ||
            !mediaType) {
            continue;
        }
        VIDEO_STREAM_CONFIG_CAPS streamCaps{};
        if (capBytes >= static_cast<int>(sizeof(streamCaps))) {
            std::memcpy(&streamCaps, caps.data(), sizeof(streamCaps));
        }
        int candidateWidth = 0;
        int candidateHeight = 0;
        REFERENCE_TIME duration = 0;
        DWORD bytes = 0;
        VideoPixelFormat format = VideoPixelFormat::Auto;
        if (VideoFormatDetails(mediaType, candidateWidth, candidateHeight,
                               duration, bytes, &format) &&
            format != VideoPixelFormat::Auto && candidateWidth == width &&
            candidateHeight == height && duration > 0) {
            const int fps = static_cast<int>(
                (10'000'000 + duration / 2) / duration);
            const bool duplicate = std::any_of(
                result.begin(), result.end(),
                [format, fps](const PixelFormatSupport& support) {
                    return support.format == format &&
                           support.selectedFps == fps;
                });
            if (!duplicate) result.push_back({format, fps});

            constexpr int kAdditionalFrameRate = 30;
            const bool thirtyFpsDuplicate = std::any_of(
                result.begin(), result.end(),
                [format](const PixelFormatSupport& support) {
                    return support.format == format &&
                           support.selectedFps == 30;
                });
            if (!thirtyFpsDuplicate &&
                FrameRateAllowedByCaps(streamCaps, kAdditionalFrameRate)) {
                result.push_back({format, kAdditionalFrameRate});
            }
        }
        DeleteMediaType(mediaType);
    }
    std::sort(
        result.begin(), result.end(),
        [](const PixelFormatSupport& left,
           const PixelFormatSupport& right) {
            if (left.format != right.format) {
                return PixelFormatRank(left.format) <
                       PixelFormatRank(right.format);
            }
            return left.selectedFps > right.selectedFps;
        });
    SafeRelease(config);
    return result;
}

}  // namespace llcv::video
