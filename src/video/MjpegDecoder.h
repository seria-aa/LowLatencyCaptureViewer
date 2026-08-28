#pragma once

#include "video/CaptureColorMetadata.h"
#include "video/VideoColor.h"

#include <dshow.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>

namespace llcv::video {

class MjpegDecoder {
public:
    using LogCallback = void (*)(const wchar_t* message);

    MjpegDecoder() = default;
    ~MjpegDecoder();
    MjpegDecoder(const MjpegDecoder&) = delete;
    MjpegDecoder& operator=(const MjpegDecoder&) = delete;

    HRESULT initialize(int width, int height, int fps,
                       const AM_MEDIA_TYPE* captureType,
                       const CaptureColorMetadata* directShowColor,
                       video_color::Override colorOverride,
                       LogCallback logCallback);

    // Returns S_OK without an output frame while the decoder is waiting for
    // enough input. If several frames become available, only the newest is
    // returned to preserve the viewer's latest-frame policy.
    HRESULT decode(IMediaSample* directShowSample, IMFMediaBuffer** output);

    LONG stride() const { return stride_; }
    video_color::Configuration colorConfiguration() const {
        return colorConfiguration_;
    }

    void reset();

private:
    static void SetColorAttribute(IMFMediaType* destination, REFGUID key,
                                  UINT32 value);
    static void CopyDirectShowColorAttributes(
        const CaptureColorMetadata& metadata, IMFMediaType* destination);
    static UINT32 ReadColorAttribute(IMFMediaType* type, REFGUID key);
    static void CopyMpegSequenceHeader(const AM_MEDIA_TYPE* captureType,
                                       IMFMediaType* destination);

    void UpdateColorConfiguration();
    HRESULT SetNv12OutputType(IMFTransform* transform);
    HRESULT PullOutput(IMFMediaBuffer** newest);
    void Log(const wchar_t* message) const;

    IMFTransform* transform_ = nullptr;
    IMFMediaType* outputType_ = nullptr;
    MFT_OUTPUT_STREAM_INFO outputInfo_{};
    DWORD bufferBytes_ = 0;
    LONG stride_ = 0;
    int width_ = 0;
    int height_ = 0;
    CaptureColorMetadata directShowColor_{};
    video_color::Override colorOverride_ = video_color::Override::Auto;
    video_color::Configuration colorConfiguration_{};
    LogCallback logCallback_ = nullptr;
    bool mfStarted_ = false;
};

}  // namespace llcv::video
