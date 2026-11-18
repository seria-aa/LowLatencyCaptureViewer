#include "recording/RecordingContainer.h"
#include "recording/RecordingStorage.h"
#include <mfapi.h>
#include <mferror.h>
#include <wrl/client.h>
#include <initializer_list>
#ifdef LLCV_RECORDING_MODERN_MP4
#include "recording/FragmentedMp4Sink.h"
#endif

namespace llcv::recording {
using Microsoft::WRL::ComPtr;
HRESULT CreateRecordingWriter(IMFByteStream* bytes, IMFAttributes* attributes,
                              IMFSinkWriter** writer, Codec codec, bool preserveColor) {
    if (!writer) return E_POINTER;
    *writer = nullptr;
    if (!bytes) return E_INVALIDARG;
    if (static_cast<int>(codec) < 0 || static_cast<int>(codec) > 2) return E_INVALIDARG;
    ComPtr<IMFAttributes> configured;
    HRESULT hr = MFCreateAttributes(&configured, 4);
    if (FAILED(hr)) return hr;
    if (attributes && FAILED(hr = attributes->CopyAllItems(configured.Get()))) return hr;
#ifdef LLCV_RECORDING_MODERN_MP4
    // Use the same hybrid MP4 path for every encoder. The native fragmented
    // H.264 sink does not finish a conventional sample index for Explorer.
    // Encoding remains in Media Foundation; only compressed packets are muxed.
    {
        ComPtr<IMFMediaSink> sink;
        if (FAILED(hr = CreateFragmentedMp4Sink(bytes, &sink,nullptr,nullptr,
                                               preserveColor?attributes:nullptr))) return hr;
        hr = MFCreateSinkWriterFromMediaSink(sink.Get(), configured.Get(), writer);
        if (FAILED(hr)) sink->Shutdown();
        return hr;
    }
#else
    if (codec != Codec::H264 || preserveColor) {
        // A playback-only build can omit the mux SDK. Never silently replace
        // an unavailable codec with a different encoder/container.
        return MF_E_TOPO_CODEC_NOT_FOUND;
    }
    if (FAILED(hr = configured->SetGUID(MF_TRANSCODE_CONTAINERTYPE, MFTranscodeContainerType_FMPEG4))) return hr;
    // Let the sink interleave audio/video into approximately one-second chunks.
    // A coded-sequence-only policy can leave AAC queued until Finalize. This is
    // a MINIMUM duration, not a promise that at most one second can be lost.
    if (FAILED(hr = configured->SetUINT64(MF_MPEG4SINK_MIN_FRAGMENT_DURATION, 10000000))) return hr;
    ComPtr<IMFByteStream> view;
    if (FAILED(hr = MFCreateMFByteStreamWrapper(bytes, &view))) return hr;
    return MFCreateSinkWriterFromURL(nullptr, view.Get(), configured.Get(), writer);
#endif
}
HRESULT CreateRecordingFile(const wchar_t* path, IMFAttributes* attributes,
                            IMFByteStream** bytes, IMFSinkWriter** writer, Codec codec,
                            const Options* options,std::shared_ptr<RecordingArchive>* archive,
                            bool preserveColor) {
    if (bytes) *bytes = nullptr;
    if (writer) *writer = nullptr;
    if (!bytes || !writer) return E_POINTER;
    if (!path || !*path) return E_INVALIDARG;
    if(archive)archive->reset();
    if(options && !Valid(*options))return E_INVALIDARG;
    if (static_cast<int>(codec) < 0 || static_cast<int>(codec) > 2) return E_INVALIDARG;
    ComPtr<IMFByteStream> file;
    const bool segmented=options&&options->splitMode!=SplitMode::None;
    if(segmented&&!archive)return E_POINTER;
#ifndef LLCV_RECORDING_MODERN_MP4
    if(segmented)return MF_E_TOPO_CODEC_NOT_FOUND;
#endif
    const auto first=segmented?RecordingPartPath(path,1):std::wstring(path);
    HRESULT hr = MFCreateFile(MF_ACCESSMODE_READWRITE, MF_OPENMODE_FAIL_IF_EXIST,
                             MF_FILEFLAGS_NONE, first.c_str(), &file);
    if (FAILED(hr)) return hr;
    ComPtr<IMFByteStream> buffered;
    if(FAILED(hr=BufferRecordingStorage(file.Get(),nullptr,&buffered)))return hr;
    file=std::move(buffered);
    if(segmented){
#ifdef LLCV_RECORDING_MODERN_MP4
        SplitPolicy split;split.basePath=path;
        if(options->splitMode==SplitMode::Time)split.duration100ns=uint64_t(options->splitSeconds)*10000000;
        else split.bytes=uint64_t(options->splitMiB)*1024*1024;
        ComPtr<IMFMediaSink> sink;
        if(FAILED(hr=CreateFragmentedMp4Sink(file.Get(),&sink,&split,archive,
                                           preserveColor?attributes:nullptr)))return hr;
        hr=MFCreateSinkWriterFromMediaSink(sink.Get(),attributes,writer);
        if(FAILED(hr)){sink->Shutdown();archive->reset();}
        if(SUCCEEDED(hr))*bytes=file.Detach();
        return hr;
#else
        return MF_E_TOPO_CODEC_NOT_FOUND;
#endif
    }
    hr = CreateRecordingWriter(file.Get(), attributes, writer, codec, preserveColor);
    if (SUCCEEDED(hr)) *bytes = file.Detach();
    return hr;
}
HRESULT ValidateCompletedRecording(const wchar_t* path) {
    if (!path || !*path) return E_INVALIDARG;
    for (const bool video : {true, false}) {
        ComPtr<IMFSourceReader> reader;
        HRESULT hr = MFCreateSourceReaderFromURL(path, nullptr, &reader);
        if (FAILED(hr)) return hr;
        const DWORD stream = static_cast<DWORD>(video ? MF_SOURCE_READER_FIRST_VIDEO_STREAM : MF_SOURCE_READER_FIRST_AUDIO_STREAM);
        ComPtr<IMFMediaType> type;
        if (FAILED(hr = reader->GetNativeMediaType(stream, 0, &type))) return hr;
        GUID major{}, subtype{};
        if (FAILED(hr = type->GetGUID(MF_MT_MAJOR_TYPE, &major)) ||
            FAILED(hr = type->GetGUID(MF_MT_SUBTYPE, &subtype))) return hr;
        if (video) {
            UINT32 width = 0, height = 0;
            if (major != MFMediaType_Video || (subtype != MFVideoFormat_H264 && subtype != MFVideoFormat_HEVC && subtype != MFVideoFormat_AV1) ||
                FAILED(MFGetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, &width, &height)) || !width || !height)
                return MF_E_INVALIDMEDIATYPE;
        } else {
            UINT32 channels = 0, rate = 0;
            if (major != MFMediaType_Audio || subtype != MFAudioFormat_AAC ||
                FAILED(type->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &channels)) ||
                FAILED(type->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &rate)) || channels != 2 || rate != 48000)
                return MF_E_INVALIDMEDIATYPE;
        }
        // Explicit native type: never introduce an optional HEVC/AV1 decoder
        // requirement merely to decide whether a saved file can be offered.
        if (FAILED(hr = reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE)) ||
            FAILED(hr = reader->SetStreamSelection(stream, TRUE)) ||
            FAILED(hr = reader->SetCurrentMediaType(stream, nullptr, type.Get()))) return hr;
        bool present = false;
        for (unsigned read = 0; read < 32; ++read) {
            ComPtr<IMFSample> sample; DWORD flags = 0; LONGLONG time = 0;
            if (FAILED(hr = reader->ReadSample(stream, 0, nullptr, &flags, &time, &sample))) return hr;
            if (flags & (MF_SOURCE_READERF_ERROR | MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED | MF_SOURCE_READERF_NATIVEMEDIATYPECHANGED))
                return MF_E_INVALID_STREAM_DATA;
            if (sample) {
                DWORD length = 0;
                if (FAILED(hr = sample->GetTotalLength(&length))) return hr;
                if (!length) return MF_E_INVALID_STREAM_DATA;
                present = true; break;
            }
            if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
        }
        if (!present) return MF_E_INVALID_STREAM_DATA;
    }
    return S_OK;
}
} // namespace llcv::recording
