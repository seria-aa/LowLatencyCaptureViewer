#include "video/DirectShowVideoFormat.h"

#include <dvdmedia.h>
#include <mfapi.h>

#include <cstdio>

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

}  // namespace

int main() {
    TestPixelFormatHelpers();
    TestVideoInfo2Details();
    TestColorMetadataMerge();
    if (failures != 0) {
        std::fprintf(stderr, "%d test(s) failed.\n", failures);
        return 1;
    }
    std::puts("DirectShow video format tests passed.");
    return 0;
}
