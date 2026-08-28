#include "video/MjpegDecoder.h"

#include <dvdmedia.h>
#include <mferror.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace llcv::video {
namespace {

template<class T>
void SafeRelease(T*& value) {
    if (!value) return;
    value->Release();
    value = nullptr;
}

}  // namespace

MjpegDecoder::~MjpegDecoder() {
    reset();
}

HRESULT MjpegDecoder::initialize(
    int width, int height, int fps, const AM_MEDIA_TYPE* captureType,
    const CaptureColorMetadata* directShowColor,
    video_color::Override colorOverride, LogCallback logCallback) {
    reset();
    if (width <= 0 || height <= 0 || fps <= 0) return E_INVALIDARG;

    HRESULT hr = MFStartup(MF_VERSION, MFSTARTUP_LITE);
    if (FAILED(hr)) return hr;
    mfStarted_ = true;
    width_ = width;
    height_ = height;
    colorOverride_ = colorOverride;
    logCallback_ = logCallback;
    if (directShowColor && directShowColor->present) {
        directShowColor_ = *directShowColor;
    }

    IMFMediaType* inputType = nullptr;
    hr = MFCreateMediaType(&inputType);
    if (SUCCEEDED(hr)) hr = inputType->SetGUID(MF_MT_MAJOR_TYPE,
                                               MFMediaType_Video);
    if (SUCCEEDED(hr)) hr = inputType->SetGUID(MF_MT_SUBTYPE,
                                               MFVideoFormat_MJPG);
    if (SUCCEEDED(hr)) hr = MFSetAttributeSize(inputType, MF_MT_FRAME_SIZE,
                                               width, height);
    if (SUCCEEDED(hr)) hr = MFSetAttributeRatio(inputType, MF_MT_FRAME_RATE,
                                                fps, 1);
    if (SUCCEEDED(hr)) hr = inputType->SetUINT32(
        MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    if (SUCCEEDED(hr)) hr = inputType->SetUINT32(
        MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
    if (SUCCEEDED(hr)) {
        CopyDirectShowColorAttributes(directShowColor_, inputType);
        CopyMpegSequenceHeader(captureType, inputType);
    }
    if (FAILED(hr)) {
        SafeRelease(inputType);
        reset();
        return hr;
    }

    MFT_REGISTER_TYPE_INFO inputInfo{};
    inputInfo.guidMajorType = MFMediaType_Video;
    inputInfo.guidSubtype = MFVideoFormat_MJPG;
    MFT_REGISTER_TYPE_INFO outputInfo{};
    outputInfo.guidMajorType = MFMediaType_Video;
    outputInfo.guidSubtype = MFVideoFormat_NV12;
    IMFActivate** activations = nullptr;
    UINT32 activationCount = 0;
    hr = MFTEnumEx(MFT_CATEGORY_VIDEO_DECODER,
                   MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_LOCALMFT |
                       MFT_ENUM_FLAG_SORTANDFILTER,
                   &inputInfo, &outputInfo, &activations, &activationCount);
    if (FAILED(hr) || activationCount == 0) {
        if (SUCCEEDED(hr)) hr = MF_E_TOPO_CODEC_NOT_FOUND;
        SafeRelease(inputType);
        if (activations) CoTaskMemFree(activations);
        reset();
        return hr;
    }

    HRESULT finalHr = MF_E_TOPO_CODEC_NOT_FOUND;
    for (UINT32 i = 0; i < activationCount; ++i) {
        IMFTransform* candidate = nullptr;
        const HRESULT activateHr = activations[i]->ActivateObject(
            IID_PPV_ARGS(&candidate));
        if (SUCCEEDED(activateHr)) {
            IMFAttributes* attributes = nullptr;
            if (SUCCEEDED(candidate->QueryInterface(IID_PPV_ARGS(&attributes)))) {
                attributes->SetUINT32(MF_LOW_LATENCY, TRUE);
                SafeRelease(attributes);
            }
            HRESULT candidateHr = candidate->SetInputType(0, inputType, 0);
            if (SUCCEEDED(candidateHr)) {
                candidateHr = SetNv12OutputType(candidate);
            }
            if (SUCCEEDED(candidateHr)) {
                candidateHr = candidate->GetOutputStreamInfo(0, &outputInfo_);
            }
            if (SUCCEEDED(candidateHr)) {
                transform_ = candidate;
                candidate = nullptr;
                UINT32 defaultStride = 0;
                if (outputType_) {
                    outputType_->GetUINT32(MF_MT_DEFAULT_STRIDE,
                                            &defaultStride);
                }
                stride_ = defaultStride
                    ? static_cast<LONG>(defaultStride)
                    : static_cast<LONG>(width_);
                bufferBytes_ = (std::max)(
                    outputInfo_.cbSize,
                    static_cast<DWORD>(width_) *
                        static_cast<DWORD>(height_) * 3u / 2u);
                transform_->ProcessMessage(
                    MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
                transform_->ProcessMessage(
                    MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
                wchar_t message[256]{};
                swprintf_s(
                    message,
                    L"[video] Media Foundation compressed decoder: MJPEG -> "
                    L"NV12, synchronous low-latency mode, output stride %ld.\n",
                    stride_);
                Log(message);
                finalHr = S_OK;
            } else {
                finalHr = candidateHr;
            }
        } else {
            finalHr = activateHr;
        }
        SafeRelease(candidate);
        activations[i]->Release();
    }
    CoTaskMemFree(activations);
    SafeRelease(inputType);
    if (FAILED(finalHr)) reset();
    return finalHr;
}

HRESULT MjpegDecoder::decode(IMediaSample* directShowSample,
                             IMFMediaBuffer** output) {
    if (!output) return E_POINTER;
    *output = nullptr;
    if (!transform_ || !directShowSample) return MF_E_NOT_INITIALIZED;

    BYTE* source = nullptr;
    const long sourceLength = directShowSample->GetActualDataLength();
    HRESULT hr = directShowSample->GetPointer(&source);
    if (FAILED(hr) || !source || sourceLength <= 0) {
        return FAILED(hr) ? hr : E_FAIL;
    }

    IMFSample* inputSample = nullptr;
    IMFMediaBuffer* inputBuffer = nullptr;
    hr = MFCreateSample(&inputSample);
    if (SUCCEEDED(hr)) {
        hr = MFCreateMemoryBuffer(static_cast<DWORD>(sourceLength),
                                  &inputBuffer);
    }
    BYTE* destination = nullptr;
    if (SUCCEEDED(hr)) hr = inputBuffer->Lock(&destination, nullptr, nullptr);
    if (SUCCEEDED(hr)) {
        memcpy(destination, source, static_cast<size_t>(sourceLength));
        inputBuffer->Unlock();
        destination = nullptr;
        hr = inputBuffer->SetCurrentLength(static_cast<DWORD>(sourceLength));
    }
    if (SUCCEEDED(hr)) hr = inputSample->AddBuffer(inputBuffer);
    REFERENCE_TIME start = 0;
    REFERENCE_TIME stop = 0;
    if (SUCCEEDED(directShowSample->GetTime(&start, &stop))) {
        inputSample->SetSampleTime(start);
        if (stop > start) inputSample->SetSampleDuration(stop - start);
    }
    if (FAILED(hr)) {
        if (destination) inputBuffer->Unlock();
        SafeRelease(inputBuffer);
        SafeRelease(inputSample);
        return hr;
    }

    IMFMediaBuffer* newest = nullptr;
    for (;;) {
        hr = transform_->ProcessInput(0, inputSample, 0);
        if (hr != MF_E_NOTACCEPTING) break;
        const HRESULT drainHr = PullOutput(&newest);
        if (drainHr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
            hr = MF_E_NOTACCEPTING;
            break;
        }
        if (FAILED(drainHr)) {
            hr = drainHr;
            break;
        }
    }
    SafeRelease(inputBuffer);
    SafeRelease(inputSample);
    if (FAILED(hr)) {
        SafeRelease(newest);
        return hr;
    }

    for (;;) {
        const HRESULT outputHr = PullOutput(&newest);
        if (outputHr == MF_E_TRANSFORM_NEED_MORE_INPUT) break;
        if (FAILED(outputHr)) {
            SafeRelease(newest);
            return outputHr;
        }
    }
    *output = newest;
    return S_OK;
}

void MjpegDecoder::reset() {
    if (transform_) {
        transform_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
        transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
    }
    SafeRelease(outputType_);
    SafeRelease(transform_);
    outputInfo_ = {};
    bufferBytes_ = 0;
    stride_ = 0;
    width_ = 0;
    height_ = 0;
    directShowColor_ = {};
    colorOverride_ = video_color::Override::Auto;
    colorConfiguration_ = {};
    logCallback_ = nullptr;
    if (mfStarted_) {
        MFShutdown();
        mfStarted_ = false;
    }
}

void MjpegDecoder::SetColorAttribute(IMFMediaType* destination, REFGUID key,
                                     UINT32 value) {
    if (destination && value != 0) destination->SetUINT32(key, value);
}

void MjpegDecoder::CopyDirectShowColorAttributes(
    const CaptureColorMetadata& metadata, IMFMediaType* destination) {
    if (!metadata.present || !destination) return;
    SetColorAttribute(destination, MF_MT_VIDEO_CHROMA_SITING,
                      metadata.chromaSubsampling);
    SetColorAttribute(destination, MF_MT_VIDEO_NOMINAL_RANGE,
                      metadata.nominalRange);
    SetColorAttribute(destination, MF_MT_YUV_MATRIX,
                      metadata.transferMatrix);
    SetColorAttribute(destination, MF_MT_VIDEO_LIGHTING,
                      metadata.lighting);
    SetColorAttribute(destination, MF_MT_VIDEO_PRIMARIES,
                      metadata.primaries);
    SetColorAttribute(destination, MF_MT_TRANSFER_FUNCTION,
                      metadata.transferFunction);
}

UINT32 MjpegDecoder::ReadColorAttribute(IMFMediaType* type, REFGUID key) {
    UINT32 value = 0;
    if (type) type->GetUINT32(key, &value);
    return value;
}

void MjpegDecoder::UpdateColorConfiguration() {
    const video_color::Metadata mf{
        ReadColorAttribute(outputType_, MF_MT_YUV_MATRIX),
        ReadColorAttribute(outputType_, MF_MT_VIDEO_NOMINAL_RANGE)};
    const video_color::Metadata directShow{
        directShowColor_.transferMatrix, directShowColor_.nominalRange};
    colorConfiguration_ = video_color::Resolve(
        true, width_, height_, mf, directShow, colorOverride_);
    wchar_t message[512]{};
    swprintf_s(
        message,
        L"[video] MJPEG color: %s · %s; matrix source=%s, range source=%s "
        L"(MF matrix=%u range=%u; DirectShow matrix=%u range=%u).\n",
        video_color::MatrixName(colorConfiguration_.matrix),
        video_color::RangeName(colorConfiguration_.range),
        video_color::SourceName(colorConfiguration_.matrixSource),
        video_color::SourceName(colorConfiguration_.rangeSource),
        mf.matrix, mf.range, directShow.matrix, directShow.range);
    Log(message);
}

void MjpegDecoder::CopyMpegSequenceHeader(
    const AM_MEDIA_TYPE* captureType, IMFMediaType* destination) {
    if (!captureType || !destination ||
        captureType->formattype != FORMAT_MPEG2Video ||
        captureType->cbFormat < FIELD_OFFSET(MPEG2VIDEOINFO,
                                             dwSequenceHeader)) {
        return;
    }
    const auto* info = reinterpret_cast<const MPEG2VIDEOINFO*>(
        captureType->pbFormat);
    const size_t available = captureType->cbFormat -
        FIELD_OFFSET(MPEG2VIDEOINFO, dwSequenceHeader);
    if (!info->cbSequenceHeader || info->cbSequenceHeader > available) return;
    destination->SetBlob(
        MF_MT_MPEG_SEQUENCE_HEADER,
        reinterpret_cast<const UINT8*>(info->dwSequenceHeader),
        info->cbSequenceHeader);
}

HRESULT MjpegDecoder::SetNv12OutputType(IMFTransform* transform) {
    SafeRelease(outputType_);
    for (DWORD index = 0;; ++index) {
        IMFMediaType* candidate = nullptr;
        HRESULT hr = transform->GetOutputAvailableType(0, index, &candidate);
        if (FAILED(hr)) return hr;
        GUID subtype{};
        const HRESULT subtypeHr = candidate->GetGUID(MF_MT_SUBTYPE, &subtype);
        if (SUCCEEDED(subtypeHr) && subtype == MFVideoFormat_NV12) {
            hr = transform->SetOutputType(0, candidate, 0);
            if (SUCCEEDED(hr)) {
                IMFMediaType* current = nullptr;
                if (SUCCEEDED(transform->GetOutputCurrentType(0, &current)) &&
                    current) {
                    SafeRelease(candidate);
                    outputType_ = current;
                } else {
                    outputType_ = candidate;
                }
                UpdateColorConfiguration();
                return S_OK;
            }
        }
        SafeRelease(candidate);
    }
}

HRESULT MjpegDecoder::PullOutput(IMFMediaBuffer** newest) {
    if (!newest) return E_POINTER;
    IMFSample* suppliedSample = nullptr;
    MFT_OUTPUT_DATA_BUFFER output{};
    if ((outputInfo_.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) == 0) {
        HRESULT hr = MFCreateSample(&suppliedSample);
        if (SUCCEEDED(hr)) {
            IMFMediaBuffer* suppliedBuffer = nullptr;
            hr = MFCreateMemoryBuffer(bufferBytes_, &suppliedBuffer);
            if (SUCCEEDED(hr)) hr = suppliedSample->AddBuffer(suppliedBuffer);
            SafeRelease(suppliedBuffer);
        }
        if (FAILED(hr)) {
            SafeRelease(suppliedSample);
            return hr;
        }
        output.pSample = suppliedSample;
    }
    DWORD status = 0;
    HRESULT hr = transform_->ProcessOutput(0, 1, &output, &status);
    SafeRelease(output.pEvents);
    if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
        SafeRelease(output.pSample);
        return SetNv12OutputType(transform_);
    }
    if (SUCCEEDED(hr) && output.pSample) {
        IMFMediaBuffer* buffer = nullptr;
        hr = output.pSample->ConvertToContiguousBuffer(&buffer);
        if (SUCCEEDED(hr)) {
            SafeRelease(*newest);
            *newest = buffer;
        }
    }
    SafeRelease(output.pSample);
    return hr;
}

void MjpegDecoder::Log(const wchar_t* message) const {
    if (logCallback_ && message) logCallback_(message);
}

}  // namespace llcv::video
