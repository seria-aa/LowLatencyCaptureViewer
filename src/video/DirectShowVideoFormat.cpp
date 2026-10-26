#include "video/DirectShowVideoFormat.h"
#include "video/HdrPolicy.h"

#include <dvdmedia.h>
#include <dxva.h>
#include <mfapi.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>
#include <limits>
#include <cstdio>

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
    // 100ns rounding at a boundary (e.g. 240fps) must not hide that rate.
    return duration >= minimum - 1 &&
        (duration <= maximum || duration - maximum <= 1);
}

AM_MEDIA_TYPE* CloneMediaType(const AM_MEDIA_TYPE* source) {
    auto* copy = static_cast<AM_MEDIA_TYPE*>(CoTaskMemAlloc(sizeof(AM_MEDIA_TYPE)));
    if (!copy) return nullptr;
    *copy = *source;
    copy->pbFormat = nullptr;
    if (source->cbFormat) {
        copy->pbFormat = static_cast<BYTE*>(CoTaskMemAlloc(source->cbFormat));
        if (!copy->pbFormat) { CoTaskMemFree(copy); return nullptr; }
        std::memcpy(copy->pbFormat, source->pbFormat, source->cbFormat);
    }
    if (copy->pUnk) copy->pUnk->AddRef();
    return copy;
}

void LogQuery(LogCallback log, const wchar_t* stage, int index, HRESULT hr) {
    if (!log) return;
    wchar_t text[192]{};
    swprintf_s(text, L"[video-caps] %s index=%d result=0x%08X\n",
               stage, index, static_cast<unsigned>(hr));
    log(text);
}

// Geometry is useful for diagnostics even when LLCV cannot decode the subtype.
// Keep header bounds checking shared with the accepted-format path.
bool ReadVideoHeader(const AM_MEDIA_TYPE* mediaType, int& width, int& height,
                     REFERENCE_TIME& frameDuration, DWORD& imageBytes) {
    if (!mediaType || !mediaType->pbFormat || mediaType->majortype != MEDIATYPE_Video) return false;
    const BITMAPINFOHEADER* bitmap = nullptr;
    frameDuration = 0;
    if (mediaType->formattype == FORMAT_VideoInfo2 &&
        mediaType->cbFormat >= sizeof(VIDEOINFOHEADER2)) {
        const auto* info = reinterpret_cast<const VIDEOINFOHEADER2*>(mediaType->pbFormat);
        bitmap = &info->bmiHeader;
        frameDuration = info->AvgTimePerFrame;
    } else if (mediaType->formattype == FORMAT_VideoInfo &&
               mediaType->cbFormat >= sizeof(VIDEOINFOHEADER)) {
        const auto* info = reinterpret_cast<const VIDEOINFOHEADER*>(mediaType->pbFormat);
        bitmap = &info->bmiHeader;
        frameDuration = info->AvgTimePerFrame;
    } else if (mediaType->formattype == FORMAT_MPEG2Video &&
               mediaType->cbFormat >= FIELD_OFFSET(MPEG2VIDEOINFO, dwSequenceHeader)) {
        const auto* info = reinterpret_cast<const MPEG2VIDEOINFO*>(mediaType->pbFormat);
        bitmap = &info->hdr.bmiHeader;
        frameDuration = info->hdr.AvgTimePerFrame;
    }
    if (!bitmap || bitmap->biHeight == (std::numeric_limits<LONG>::min)()) return false;
    width = static_cast<int>(bitmap->biWidth);
    height = std::abs(static_cast<int>(bitmap->biHeight));
    imageBytes = bitmap->biSizeImage;
    return width > 0 && height > 0;
}

std::wstring SubtypeName(const GUID& subtype) {
    GUID fourccShape = MEDIASUBTYPE_YUY2;
    fourccShape.Data1 = subtype.Data1;
    if (subtype == fourccShape) {
        wchar_t fourcc[5]{};
        bool printable = true;
        for (unsigned i = 0; i != 4; ++i) {
            const auto value = (subtype.Data1 >> (i * 8)) & 0xff;
            printable &= value >= 0x20 && value <= 0x7e;
            fourcc[i] = static_cast<wchar_t>(value);
        }
        if (printable) return fourcc;
    }
    // RGB and vendor-defined GUIDs must not be mislabeled as FourCC values.
    wchar_t guid[40]{};
    StringFromGUID2(subtype, guid, static_cast<int>(std::size(guid)));
    return guid;
}

void LogRejectedMode(LogCallback log, int index, const AM_MEDIA_TYPE* mediaType,
                     const wchar_t* reason) {
    if (!log || !mediaType) return;
    int width = 0, height = 0;
    REFERENCE_TIME duration = 0;
    DWORD bytes = 0;
    const bool header = ReadVideoHeader(mediaType, width, height, duration, bytes);
    wchar_t guid[40]{};
    StringFromGUID2(mediaType->subtype, guid, static_cast<int>(std::size(guid)));
    wchar_t text[384]{};
    swprintf_s(text, L"[video-caps] rejected entry=%d reason=%s subtype=%s guid=%s "
        L"size=%dx%d duration=%lld (100ns) header=%s\n", index, reason,
        SubtypeName(mediaType->subtype).c_str(), guid, width, height, duration,
        header ? L"valid" : L"unavailable");
    log(text);
}

void LogMode(LogCallback log, int index, VideoPixelFormat format, int width,
             int height, REFERENCE_TIME duration, const VIDEO_STREAM_CONFIG_CAPS& caps) {
    if (!log) return;
    wchar_t text[256]{};
    swprintf_s(text, L"[video-caps] entry=%d %s %dx%d duration=%lld interval=%lld~%lld (100ns)\n",
        index, PixelFormatName(format), width, height, duration,
        caps.MinFrameInterval, caps.MaxFrameInterval);
    log(text);
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
    if (!mediaType || !mediaType->pbFormat || mediaType->majortype != MEDIATYPE_Video) return false;
    const VideoPixelFormat detected = PixelFormatFromSubtype(mediaType->subtype);
    if (detected == VideoPixelFormat::Auto) return false;
    if (pixelFormat) *pixelFormat = detected;
    return ReadVideoHeader(mediaType, width, height, frameDuration, imageBytes);
}

HRESULT ValidateVideoLayout(const AM_MEDIA_TYPE* mediaType,
    int expectedWidth, int expectedHeight, VideoPixelFormat expectedFormat,
    DWORD& imageBytes, UINT32& stride, int& fps) {
    int width = 0, height = 0;
    REFERENCE_TIME duration = 0;
    DWORD bytes = 0;
    VideoPixelFormat format = VideoPixelFormat::Auto;
    if (!VideoFormatDetails(mediaType, width, height, duration, bytes, &format) ||
        width != expectedWidth || height != expectedHeight || format != expectedFormat ||
        duration < 0 || duration > 10'000'000 || (duration == 0 && fps <= 0))
        return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    UINT32 rowPitch = 0;
    if (!IsCompressedVideoFormat(format)) {
        if ((width & 1) || (format != VideoPixelFormat::Yuy2 && (height & 1)))
            return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
        const uint64_t rows = format == VideoPixelFormat::Yuy2 ? height : uint64_t(height) * 3 / 2;
        const uint64_t minimumPitch = uint64_t(width) * (format == VideoPixelFormat::Nv12 ? 1 : 2);
        const uint64_t minimumBytes = rows * minimumPitch;
        if (minimumBytes > (std::numeric_limits<LONG>::max)() ||
            (bytes && (bytes < minimumBytes || bytes > static_cast<DWORD>((std::numeric_limits<LONG>::max)()))))
            return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
        if (!bytes) bytes = static_cast<DWORD>(minimumBytes);
        const uint64_t pitch = bytes / rows;
        if (pitch < minimumPitch || (format != VideoPixelFormat::Nv12 && (pitch & 1)))
            return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
        rowPitch = static_cast<UINT32>(pitch);
    }
    imageBytes = bytes;
    stride = rowPitch;
    // A connected type may omit frame timing; preserve the negotiated rate.
    if (duration > 0) fps = static_cast<int>((10'000'000 + duration / 2) / duration);
    return S_OK;
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

bool ReadVideoSampleFormat(const AM_MEDIA_TYPE* mediaType, int fallbackFps,
                           VideoSampleFormat& result) {
    result = {};
    VideoSampleFormat next{};
    REFERENCE_TIME duration = 0;
    if (!VideoFormatDetails(mediaType, next.width, next.height, duration,
                            next.bytes, &next.format)) return false;
    next.fps = fallbackFps;
    if (FAILED(ValidateVideoLayout(mediaType, next.width, next.height, next.format,
                                  next.bytes, next.stride, next.fps))) return false;
    const VIDEOINFOHEADER2* info2 = nullptr;
    if (mediaType->formattype == FORMAT_VideoInfo2)
        info2 = reinterpret_cast<const VIDEOINFOHEADER2*>(mediaType->pbFormat);
    else if (mediaType->formattype == FORMAT_MPEG2Video)
        info2 = &reinterpret_cast<const MPEG2VIDEOINFO*>(mediaType->pbFormat)->hdr;
    if (info2) {
        next.source = info2->rcSource; next.target = info2->rcTarget;
        next.interlace = info2->dwInterlaceFlags;
        next.aspectX = info2->dwPictAspectRatioX; next.aspectY = info2->dwPictAspectRatioY;
    } else {
        const auto* info = reinterpret_cast<const VIDEOINFOHEADER*>(mediaType->pbFormat);
        next.source = info->rcSource; next.target = info->rcTarget;
    }
    const auto normalizeRect = [&](RECT& rect) {
        if (IsRectEmpty(&rect)) rect = {0, 0, next.width, next.height};
    };
    normalizeRect(next.source); normalizeRect(next.target);
    if (!next.aspectX || !next.aspectY) {
        next.aspectX = next.width; next.aspectY = next.height;
    }
    ExtractDirectShowColorMetadata(mediaType, next.color);
    next.valid = true;
    result = next;
    return true;
}

bool MatchesVideoSampleFormat(const AM_MEDIA_TYPE* mediaType,
                              const VideoSampleFormat& expected) {
    VideoSampleFormat actual{};
    if (!expected.valid || !ReadVideoSampleFormat(mediaType, expected.fps, actual)) return false;
    if (actual.format == VideoPixelFormat::P010)
        actual.color = llcv::hdr::ConnectedMetadata(expected.color, actual.color);
    else {
        auto merged = expected.color;
        MergeDirectShowColorMetadata(merged, actual.color);
        actual.color = merged;
    }
    const auto normalizedColor = [](const VideoSampleFormat& value) {
        auto color = value.color;
        if (value.format == VideoPixelFormat::P010) {
            const auto hdr = llcv::hdr::ResolveInput(color, false);
            if (hdr.kind == llcv::hdr::InputKind::Hdr10) {
                // Untagged assumed HDR -> explicitly confirmed identical HDR
                // is not a format change. Match the production default policy.
                color.primaries = 9; color.transferFunction = 15;
                color.transferMatrix = 4; color.nominalRange = 2;
                color.chromaSubsampling = hdr.colorSpace ==
                    DXGI_COLOR_SPACE_YCBCR_STUDIO_G2084_LEFT_P2020 ? 5u : 7u;
            }
        }
        return color;
    };
    const auto a = normalizedColor(actual);
    const auto b = normalizedColor(expected);
    // Ignore allocation hints for MJPEG (individual compressed sizes vary),
    // diagnostic lighting/reserved flags, and equivalent aspect-ratio spelling.
    // Unknown fields retain startup's effective metadata, as on connection.
    // Explicitly changed color declarations must not use stale interpretation.
    return actual.format == expected.format && actual.width == expected.width &&
        actual.height == expected.height && actual.fps == expected.fps &&
        actual.stride == expected.stride &&
        (IsCompressedVideoFormat(actual.format) || actual.bytes == expected.bytes) &&
        actual.interlace == expected.interlace &&
        EqualRect(&actual.source, &expected.source) && EqualRect(&actual.target, &expected.target) &&
        uint64_t(actual.aspectX) * expected.aspectY == uint64_t(expected.aspectX) * actual.aspectY &&
        a.chromaSubsampling == b.chromaSubsampling && a.nominalRange == b.nominalRange &&
        a.transferMatrix == b.transferMatrix && a.primaries == b.primaries &&
        a.transferFunction == b.transferFunction;
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
    if (FAILED(hr) || count < 0 ||
        (count > 0 && capBytes < static_cast<int>(sizeof(VIDEO_STREAM_CONFIG_CAPS)))) {
        SafeRelease(config);
        return false;
    }
    std::vector<BYTE> caps(capBytes > 0 ? static_cast<size_t>(capBytes) : 1);
    bool found = false;
    for (int i = 0; i < count && !found; ++i) {
        AM_MEDIA_TYPE* candidate = nullptr;
        if (FAILED(config->GetStreamCaps(i, &candidate, caps.data())) ||
            !candidate) {
            DeleteMediaType(candidate);
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

bool IsRequestedFrameRate(const AM_MEDIA_TYPE* mediaType, int fps) {
    if (fps <= 0 || fps > 10'000'000) return false;
    int width = 0, height = 0;
    REFERENCE_TIME duration = 0;
    DWORD bytes = 0;
    const REFERENCE_TIME wantedDuration = FrameDurationForRate(fps);
    // Preserve the established 30/40fps tolerance. At higher rates a fixed
    // 100us window can accept a different nominal rate (e.g. 239 vs 240).
    // 0.2% still accepts 1000/1001 timing such as 59.94 and 119.88fps.
    const REFERENCE_TIME tolerance = fps == 30 || fps == 40 ? 1000 :
        std::clamp<REFERENCE_TIME>(wantedDuration / 500, 1, 1000);
    return VideoFormatDetails(mediaType, width, height, duration, bytes) &&
        duration > 0 && duration >= wantedDuration - tolerance &&
        duration <= wantedDuration + tolerance;
}

HRESULT ConfigureVideoPin(
    IPin* videoPin, int wantedWidth, int wantedHeight, int wantedFps,
    VideoPixelFormat wantedFormat, DWORD& imageBytes, UINT32& stride,
    int& configuredFps, VideoPixelFormat& configuredFormat,
    LogCallback logCallback, bool requestExactFps,
    CaptureColorMetadata* acceptedCandidateColor, bool allowUnlistedFps) {
    if (acceptedCandidateColor) *acceptedCandidateColor = {};
    if (!videoPin) return E_POINTER;
    if (requestExactFps && (wantedFps <= 0 || wantedFps > 10'000'000)) return E_INVALIDARG;
    if (allowUnlistedFps && !requestExactFps) return E_INVALIDARG;
    IAMStreamConfig* config = nullptr;
    HRESULT hr = videoPin->QueryInterface(IID_PPV_ARGS(&config));
    if (FAILED(hr)) return hr;

    int count = 0;
    int capBytes = 0;
    hr = config->GetNumberOfCapabilities(&count, &capBytes);
    if (FAILED(hr) || count < 0 ||
        capBytes < static_cast<int>(sizeof(VIDEO_STREAM_CONFIG_CAPS))) {
        LogQuery(logCallback, L"configure/count", count, FAILED(hr) ? hr : E_FAIL);
        SafeRelease(config);
        return FAILED(hr) ? hr : E_FAIL;
    }

    struct FormatCandidate {
        AM_MEDIA_TYPE* mediaType = nullptr;
        int fps = 0;
        DWORD imageBytes = 0;
        VideoPixelFormat format = VideoPixelFormat::Nv12;
        bool unlistedProbe = false;
    };
    std::vector<BYTE> caps(static_cast<size_t>(capBytes));
    std::vector<FormatCandidate> candidates;
    for (int i = 0; i < count; ++i) {
        AM_MEDIA_TYPE* candidate = nullptr;
        hr = config->GetStreamCaps(i, &candidate, caps.data());
        if (FAILED(hr) || !candidate) {
            LogQuery(logCallback, L"configure/entry", i, FAILED(hr) ? hr : E_FAIL);
            DeleteMediaType(candidate);
            continue;
        }

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
            height == wantedHeight && duration >= 0) {
            LogMode(logCallback, i, format, width, height, duration, streamCaps);
            const bool rateAllowed = FrameRateAllowedByCaps(streamCaps, wantedFps);
            if (wantedFps > 0 && fps != wantedFps &&
                (rateAllowed || (requestExactFps && allowUnlistedFps))) {
                auto* adjusted = CloneMediaType(candidate);
                if (adjusted && SetVideoFrameDuration(adjusted, FrameDurationForRate(wantedFps)))
                    candidates.push_back({adjusted, wantedFps, bytes, format, !rateAllowed});
                else DeleteMediaType(adjusted);
            }
            // A zero default duration is not an executable fallback. The
            // interval-derived requested candidate above has explicit timing.
            if (fps > 0) {
                candidates.push_back({candidate, fps, bytes, format});
                continue;
            }
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
            // A supported exact-rate format wins over an unlisted timing probe
            // in another format. Auto's normal format/fallback order is intact.
            if (a.unlistedProbe != b.unlistedProbe)
                return !a.unlistedProbe;
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
    bool requestedRateAdvertised = false;
    for (const auto& candidate : candidates)
        requestedRateAdvertised |= candidate.fps == wantedFps && !candidate.unlistedProbe;
    if (!requestedRateAdvertised) LogQuery(logCallback, L"requested-rate-not-advertised", wantedFps,
                                          HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED));
    for (auto& candidate : candidates) {
        // An explicit selection must never substitute another rate. Unlisted
        // requests are separate and must not contaminate advertised caps.
        if (requestExactFps && candidate.fps != wantedFps) continue;
        DWORD candidateBytes = 0;
        UINT32 candidateStride = 0;
        int candidateFps = candidate.fps;
        hr = ValidateVideoLayout(candidate.mediaType, wantedWidth, wantedHeight,
            candidate.format, candidateBytes, candidateStride, candidateFps);
        if (FAILED(hr)) {
            LogQuery(logCallback, L"invalid-candidate-layout/fps", candidate.fps, hr);
            continue;
        }
        hr = config->SetFormat(candidate.mediaType);
        LogQuery(logCallback, L"SetFormat/fps", candidate.fps, hr);
        if (SUCCEEDED(hr) && requestExactFps) {
            AM_MEDIA_TYPE* actual = nullptr;
            hr = config->GetFormat(&actual);
            if (SUCCEEDED(hr)) {
                hr = IsRequestedFrameRate(actual, wantedFps)
                    ? ValidateVideoLayout(actual, wantedWidth, wantedHeight,
                        candidate.format, candidateBytes, candidateStride, candidateFps)
                    : kRequestedFrameRateMismatch;
            }
            DeleteMediaType(actual);
            LogQuery(logCallback, L"exact-fps/readback", wantedFps, hr);
        }
        if (SUCCEEDED(hr)) {
            selected = &candidate;
            imageBytes = candidateBytes;
            stride = candidateStride;
            configuredFps = candidateFps;
            configuredFormat = candidate.format;
            // Retimed modes may not exist in GetStreamCaps, and GetFormat can
            // omit their color flags. Retain only the accepted candidate's
            // hints; active/connected metadata still takes precedence later.
            if (acceptedCandidateColor) {
                ExtractDirectShowColorMetadata(
                    candidate.mediaType, *acceptedCandidateColor);
            }
            break;
        }
    }

    if (selected) {
        if (SUCCEEDED(hr) && configuredFps != wantedFps && logCallback) {
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
    if (SUCCEEDED(hr) && !*mediaType) hr = HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    SafeRelease(config);
    return hr;
}

std::vector<PixelFormatSupport> ProbePixelFormats(
    IPin* videoPin, int width, int height, HRESULT* queryStatus, LogCallback logCallback,
    VideoCapabilityDiagnostics* diagnostics) {
    std::vector<PixelFormatSupport> result;
    if (queryStatus) *queryStatus = S_OK;
    if (diagnostics) *diagnostics = {};
    if (!videoPin) { if (queryStatus) *queryStatus = E_POINTER; return result; }
    IAMStreamConfig* config = nullptr;
    HRESULT hr = videoPin->QueryInterface(IID_PPV_ARGS(&config));
    int count = 0;
    int capBytes = 0;
    if (SUCCEEDED(hr)) hr = config->GetNumberOfCapabilities(&count, &capBytes);
    if (SUCCEEDED(hr) && ((count > 0 && capBytes < static_cast<int>(sizeof(VIDEO_STREAM_CONFIG_CAPS))) || count < 0))
        hr = E_FAIL;
    if (FAILED(hr)) {
        if (queryStatus) *queryStatus = hr;
        LogQuery(logCallback, L"probe/count", count, hr);
        SafeRelease(config);
        return result;
    }
    std::vector<BYTE> caps(capBytes > 0 ? static_cast<size_t>(capBytes) : 1);
    for (int i = 0; SUCCEEDED(hr) && i < count; ++i) {
        AM_MEDIA_TYPE* mediaType = nullptr;
        const HRESULT entryHr = config->GetStreamCaps(i, &mediaType, caps.data());
        if (FAILED(entryHr) || !mediaType) {
            if (queryStatus) *queryStatus = FAILED(entryHr) ? entryHr : E_FAIL;
            LogQuery(logCallback, L"probe/entry", i, FAILED(entryHr) ? entryHr : E_FAIL);
            LogRejectedMode(logCallback, i, mediaType, L"query-failed");
            DeleteMediaType(mediaType);
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
        const VideoPixelFormat format = PixelFormatFromSubtype(mediaType->subtype);
        const bool knownVideoType = mediaType->majortype == MEDIATYPE_Video &&
            format != VideoPixelFormat::Auto;
        const bool readableHeader = ReadVideoHeader(mediaType, candidateWidth, candidateHeight, duration, bytes);
        if (diagnostics && readableHeader && candidateWidth == width && candidateHeight == height) {
            ++diagnostics->matchingResolutionTypes;
            if (!knownVideoType) {
                ++diagnostics->unsupportedMatchingTypes;
                const auto name = SubtypeName(mediaType->subtype);
                auto& names = diagnostics->unsupportedFormatNames;
                if (std::find(names.begin(), names.end(), name) == names.end()) names.push_back(name);
            }
        }
        if (!knownVideoType) {
            LogRejectedMode(logCallback, i, mediaType,
                mediaType->majortype == MEDIATYPE_Video ? L"unsupported-subtype" : L"not-video");
            DeleteMediaType(mediaType);
            continue;
        }
        if (!readableHeader) {
            const HRESULT invalid = HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
            if (queryStatus) *queryStatus = invalid;
            LogQuery(logCallback, L"probe/invalid-format", i, invalid);
            LogRejectedMode(logCallback, i, mediaType, L"invalid-header");
        } else if (candidateWidth == width && candidateHeight == height) {
            LogMode(logCallback, i, format, candidateWidth, candidateHeight, duration, streamCaps);
            const int fps = duration > 0 ? static_cast<int>(
                (10'000'000 + duration / 2) / duration) : 0;
            UINT32 checkedStride = 0;
            // Validate geometry even if only the separate interval range is
            // present. This sentinel is never exposed as an advertised rate.
            int checkedFps = fps > 0 ? fps : 1;
            const bool hasTiming = duration > 0 || (duration == 0 &&
                streamCaps.MinFrameInterval > 0 && streamCaps.MaxFrameInterval > 0);
            const HRESULT layoutHr = hasTiming ? ValidateVideoLayout(mediaType, width, height,
                format, bytes, checkedStride, checkedFps) : HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
            if (FAILED(layoutHr)) {
                if (queryStatus) *queryStatus = layoutHr;
                LogQuery(logCallback, L"probe/invalid-layout", i, layoutHr);
                LogRejectedMode(logCallback, i, mediaType, L"invalid-layout-or-timing");
                DeleteMediaType(mediaType);
                continue;
            }
            const bool duplicate = std::any_of(
                result.begin(), result.end(),
                [format, fps](const PixelFormatSupport& support) {
                    return support.format == format &&
                           support.selectedFps == fps;
                });
            if (fps > 0 && !duplicate) result.push_back({format, fps});

            for (const int rate : {30, 60, 90, 100, 120, 144, 165, 180, 200, 240}) {
                const bool duplicateRate = std::any_of(result.begin(), result.end(),
                    [format, rate](const PixelFormatSupport& support) {
                        return support.format == format && support.selectedFps == rate;
                    });
                if (!duplicateRate && FrameRateAllowedByCaps(streamCaps, rate))
                    result.push_back({format, rate});
            }
        } else LogRejectedMode(logCallback, i, mediaType, L"resolution-mismatch");
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
