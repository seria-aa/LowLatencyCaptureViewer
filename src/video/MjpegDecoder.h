#pragma once

#include "video/CaptureColorMetadata.h"
#include "video/VideoColor.h"
#include "video/MediaBufferPool.h"

#include <dshow.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <optional>

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
                       video_color::MatrixOverride colorOverride,
                       LogCallback logCallback,
                       video_color::RangeOverride rangeOverride = video_color::RangeOverride::Auto);

    // Returns S_OK without an output frame while the decoder is waiting for
    // enough input. If several frames become available, only the newest is
    // returned to preserve the viewer's latest-frame policy.
    HRESULT decode(IMediaSample* directShowSample, IMFMediaBuffer** output);
    // Recorder-owned compressed bytes; no driver sample is retained.
    HRESULT decode(const BYTE* source, DWORD length, std::optional<LONGLONG> time,
                   LONGLONG duration, IMFMediaBuffer** output,
                   bool requireSingleFrame = false);

    LONG stride() const { return stride_; }
    bool validOutputBuffer(DWORD capacity, DWORD length) const {
        // NV12 upload reads the full luma and interleaved chroma planes.
        return width_ > 0 && height_ > 0 && stride_ >= width_ &&
            length <= capacity && static_cast<uint64_t>(length) >=
                static_cast<uint64_t>(stride_) *
                    (static_cast<uint64_t>(height_) + (static_cast<uint64_t>(height_) + 1) / 2);
    }
    video_color::Configuration colorConfiguration() const {
        return colorConfiguration_;
    }

    void reset();
    MediaBufferPool::Statistics inputAllocations() const { return inputBuffers_.Stats(); }
    MediaBufferPool::Statistics outputAllocations() const { return outputBuffers_.Stats(); }

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
    HRESULT PullOutput(IMFMediaBuffer** newest, unsigned* frameCount = nullptr);
    void Log(const wchar_t* message) const;

    IMFTransform* transform_ = nullptr;
    IMFMediaType* outputType_ = nullptr;
    MFT_OUTPUT_STREAM_INFO outputInfo_{};
    MFT_INPUT_STREAM_INFO inputInfo_{};
    MediaBufferPool inputBuffers_, outputBuffers_;
    DWORD bufferBytes_ = 0;
    LONG stride_ = 0;
    int width_ = 0;
    int height_ = 0;
    CaptureColorMetadata directShowColor_{};
    video_color::MatrixOverride colorOverride_ = video_color::MatrixOverride::Auto;
    video_color::RangeOverride rangeOverride_ = video_color::RangeOverride::Auto;
    video_color::Configuration colorConfiguration_{};
    LogCallback logCallback_ = nullptr;
    bool mfStarted_ = false;
};

}  // namespace llcv::video
