#include "recording/FragmentedMp4Sink.h"
#include "recording/RecordingSplitBoundary.h"
#include "recording/RecordingStorage.h"
#include <mfapi.h>
#include <mferror.h>
#include <wrl.h>
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <vector>
#include <deque>
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/bsf.h>
#include <libavutil/channel_layout.h>
#include <libavutil/mem.h>
}

namespace llcv::recording {
namespace {
using Microsoft::WRL::ComPtr;
using Microsoft::WRL::Make;
using Microsoft::WRL::RuntimeClass;
using Microsoft::WRL::RuntimeClassFlags;
using Microsoft::WRL::ClassicCom;
constexpr AVRational kMfTimeBase{1, 10000000};
constexpr size_t kMaxPendingBytes = 32 * 1024 * 1024;
constexpr size_t kMaxPendingPackets = 512;
constexpr DWORD kMaxSampleBytes = 16 * 1024 * 1024;
constexpr UINT32 kMaxCodecHeaderBytes = 1024 * 1024;
class Sink;
struct PacketFree { void operator()(AVPacket* p) const { av_packet_free(&p); } };
using Packet = std::unique_ptr<AVPacket, PacketFree>;
struct StreamInfo {
    DWORD id = 0;
    ComPtr<IMFMediaType> type;
    AVStream* output = nullptr;
    AVBSFContext* extractor = nullptr;
    bool seen = false;
    int64_t lastDts = AV_NOPTS_VALUE;
    Packet pendingVideo;
    std::vector<uint8_t> codecHeader;
    ~StreamInfo() { av_bsf_free(&extractor); }
};

// Shared independently of COM objects, so streams cannot keep a reference cycle
// alive when an IMFSinkWriter releases its externally supplied sink.
struct State : RecordingArchive {
    std::recursive_mutex mutex;
    Sink* owner = nullptr;
    ComPtr<IMFByteStream> bytes;
    ComPtr<IMFAttributes> videoAttributes;
    std::vector<std::unique_ptr<StreamInfo>> streams;
    std::vector<Packet> pending;
    size_t pendingBytes = 0;
    AVFormatContext* format = nullptr;
    AVIOContext* io = nullptr;
    HRESULT storageError = S_OK;
    HRESULT failure = S_OK;
    bool shutdown = false, header = false, finalizing = false, finalized = false;
    SplitPolicy split;
    unsigned part=1;
    int64_t segmentOrigin=0;
    bool segmentHasVideo=false,trailerWritten=false;
    std::deque<Packet> videoPackets,audioPackets;
    size_t queuedBytes=0;
    HRESULT Flush() override { std::lock_guard lock(mutex);return bytes?bytes->Flush():MF_E_SHUTDOWN; }
    HRESULT RequestFlush() override { std::lock_guard lock(mutex);return bytes?RequestRecordingFlush(bytes.Get()):MF_E_SHUTDOWN; }
    HRESULT Close() override { std::lock_guard lock(mutex);return bytes?bytes->Close():S_OK; }
    std::wstring CurrentPath() override { std::lock_guard lock(mutex);return RecordingPartPath(split.basePath,part); }
    ~State() { ClearMux(); }
    void ClearMux() {
        pending.clear(); pendingBytes = 0;
        videoPackets.clear();audioPackets.clear();queuedBytes=0;
        if (format) { format->pb = nullptr; avformat_free_context(format); format = nullptr; }
        if (io) { av_freep(&io->buffer); avio_context_free(&io); }
        for (auto& s : streams) { s->output = nullptr; s->pendingVideo.reset(); av_bsf_free(&s->extractor); }
    }
    HRESULT Error(int error) const {
        if (FAILED(storageError)) return storageError;
        if (error >= 0) return S_OK;
        if (error == AVERROR(ENOMEM)) return E_OUTOFMEMORY;
        if (error == AVERROR(ENOSPC)) return HRESULT_FROM_WIN32(ERROR_DISK_FULL);
        if (error == AVERROR(EINVAL) || error == AVERROR_INVALIDDATA) return MF_E_INVALID_STREAM_DATA;
        return E_FAIL;
    }
    StreamInfo* Find(DWORD id) {
        for (auto& s : streams) if (s->id == id) return s.get();
        return nullptr;
    }
    static int Write(void* opaque, const uint8_t* data, int size) {
        auto& s = *static_cast<State*>(opaque);
        if (FAILED(s.storageError)) return AVERROR(EIO);
        ULONG done = 0;
        const HRESULT hr = s.bytes->Write(data, static_cast<ULONG>(size), &done);
        if (FAILED(hr) || done != static_cast<ULONG>(size)) {
            s.storageError = FAILED(hr) ? hr : HRESULT_FROM_WIN32(ERROR_WRITE_FAULT);
            return AVERROR(EIO);
        }
        return size;
    }
    static int64_t Seek(void* opaque, int64_t offset, int origin) {
        auto& s = *static_cast<State*>(opaque);
        if (FAILED(s.storageError)) return AVERROR(EIO);
        QWORD position = 0;
        HRESULT hr = S_OK;
        origin &= ~AVSEEK_FORCE;
        if (origin == AVSEEK_SIZE) hr = s.bytes->GetLength(&position);
        else if (origin == SEEK_SET && offset >= 0) {
            hr = s.bytes->SetCurrentPosition(static_cast<QWORD>(offset));
            position = static_cast<QWORD>(offset);
        } else if (origin == SEEK_CUR) hr = s.bytes->Seek(msoCurrent, offset, 0, &position);
        else if (origin == SEEK_END) {
            hr = s.bytes->GetLength(&position);
            if (SUCCEEDED(hr)) {
                if (position > INT64_MAX || offset < -static_cast<int64_t>(position) ||
                    (offset > 0 && position > static_cast<QWORD>(INT64_MAX - offset))) hr = E_INVALIDARG;
                else {
                    position = static_cast<QWORD>(static_cast<int64_t>(position) + offset);
                    hr = s.bytes->SetCurrentPosition(position);
                }
            }
        } else hr = E_INVALIDARG;
        if (FAILED(hr)) { s.storageError = hr; return AVERROR(EIO); }
        return position <= INT64_MAX ? static_cast<int64_t>(position) : AVERROR(EINVAL);
    }
    HRESULT Initialize();
    HRESULT StartHeader();
    HRESULT Process(DWORD id, IMFSample* sample);
    HRESULT WritePacket(Packet packet);
    HRESULT MuxPacket(Packet packet);
    HRESULT MuxRaw(Packet packet);
    HRESULT DrainSegments(bool finish);
    HRESULT RollSegment(int64_t origin);
    HRESULT Finalize();
};

HRESULT CopyType(IMFMediaType* from, IMFMediaType** result, IMFAttributes* videoFallback=nullptr) {
    ComPtr<IMFMediaType> copy;
    HRESULT hr = MFCreateMediaType(&copy);
    if (SUCCEEDED(hr)) hr = from->CopyAllItems(copy.Get());
    GUID major{};
    if (SUCCEEDED(hr) && videoFallback &&
        SUCCEEDED(copy->GetGUID(MF_MT_MAJOR_TYPE, &major)) && major == MFMediaType_Video) {
        // SinkWriter can drop optional color attributes before the sink first
        // sees the encoder's type. Preserve the caller's interpretation only
        // where the encoder did not explicitly report a value.
        for (const auto& key : {MF_MT_YUV_MATRIX, MF_MT_VIDEO_NOMINAL_RANGE,
                               MF_MT_VIDEO_PRIMARIES, MF_MT_TRANSFER_FUNCTION}) {
            UINT32 negotiatedValue = 0, fallback = 0;
            if ((FAILED(copy->GetUINT32(key, &negotiatedValue)) || !negotiatedValue) &&
                SUCCEEDED(videoFallback->GetUINT32(key, &fallback)) && fallback &&
                FAILED(hr = copy->SetUINT32(key, fallback))) return hr;
        }
    }
    if (SUCCEEDED(hr)) *result = copy.Detach();
    return hr;
}
HRESULT CheckType(IMFMediaType* type) {
    if (!type) return E_POINTER;
    GUID major{}, subtype{};
    if (FAILED(type->GetGUID(MF_MT_MAJOR_TYPE, &major)) || FAILED(type->GetGUID(MF_MT_SUBTYPE, &subtype)))
        return MF_E_INVALIDMEDIATYPE;
    if (major == MFMediaType_Video && (subtype == MFVideoFormat_H264 || subtype == MFVideoFormat_HEVC || subtype == MFVideoFormat_AV1)) {
        UINT32 width = 0, height = 0, n = 0, d = 0;
        if (FAILED(MFGetAttributeSize(type, MF_MT_FRAME_SIZE, &width, &height)) ||
            FAILED(MFGetAttributeRatio(type, MF_MT_FRAME_RATE, &n, &d)) ||
            !width || !height || width > 16384 || height > 16384 || !n || !d || n > INT_MAX || d > INT_MAX)
            return MF_E_INVALIDMEDIATYPE;
        return S_OK;
    }
    if (major == MFMediaType_Audio && subtype == MFAudioFormat_AAC) {
        UINT32 rate = 0, channels = 0, payload = 0;
        if (FAILED(type->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &rate)) ||
            FAILED(type->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &channels)) ||
            (rate != 48000 && rate != 44100) || channels != 2) return MF_E_INVALIDMEDIATYPE;
        const HRESULT hr = type->GetUINT32(MF_MT_AAC_PAYLOAD_TYPE, &payload);
        return (SUCCEEDED(hr) && payload != 0) ? MF_E_INVALIDMEDIATYPE : S_OK;
    }
    return MF_E_INVALIDMEDIATYPE;
}
HRESULT SetExtra(AVCodecParameters* p, const uint8_t* data, size_t size) {
    if (!size || size > kMaxCodecHeaderBytes) return MF_E_INVALIDMEDIATYPE;
    auto* copy = static_cast<uint8_t*>(av_mallocz(size + AV_INPUT_BUFFER_PADDING_SIZE));
    if (!copy) return E_OUTOFMEMORY;
    std::memcpy(copy, data, size);
    av_freep(&p->extradata); p->extradata = copy; p->extradata_size = static_cast<int>(size);
    return S_OK;
}
HRESULT TypeExtra(IMFMediaType* type, const GUID& key, AVCodecParameters* p, UINT32 skip = 0) {
    UINT32 size = 0;
    HRESULT hr = type->GetBlobSize(key, &size);
    if (hr == MF_E_ATTRIBUTENOTFOUND) return S_FALSE;
    if (FAILED(hr)) return hr;
    if (size <= skip || size > kMaxCodecHeaderBytes) return MF_E_INVALIDMEDIATYPE;
    std::vector<uint8_t> data(size);
    hr = type->GetBlob(key, data.data(), size, nullptr);
    return FAILED(hr) ? hr : SetExtra(p, data.data() + skip, size - skip);
}
HRESULT State::Initialize() {
    if (format) return S_OK;
    if (streams.empty() || streams.size() > 2) return MF_E_INVALIDREQUEST;
    int result = avformat_alloc_output_context2(&format, nullptr, "mp4", nullptr);
    if (result < 0 || !format) return Error(result < 0 ? result : AVERROR(ENOMEM));
    auto* buffer = static_cast<unsigned char*>(av_malloc(32768));
    if (!buffer) return E_OUTOFMEMORY;
    io = avio_alloc_context(buffer, 32768, 1, this, nullptr, &Write, &Seek);
    if (!io) { av_free(buffer); return E_OUTOFMEMORY; }
    format->pb = io;
    format->flags |= AVFMT_FLAG_CUSTOM_IO;
    format->avoid_negative_ts = AVFMT_AVOID_NEG_TS_DISABLED;
    // Bound libavformat's interleave queue if an encoder stops producing.
    format->max_interleave_delta = 1000000;
    for (auto& holder : streams) {
        auto& s = *holder;
        HRESULT hr = CheckType(s.type.Get());
        if (FAILED(hr)) return hr;
        s.output = avformat_new_stream(format, nullptr);
        if (!s.output) return E_OUTOFMEMORY;
        auto* p = s.output->codecpar;
        GUID major{}, subtype{};
        s.type->GetGUID(MF_MT_MAJOR_TYPE, &major); s.type->GetGUID(MF_MT_SUBTYPE, &subtype);
        UINT32 bitrate = 0;
        if (SUCCEEDED(s.type->GetUINT32(MF_MT_AVG_BITRATE, &bitrate))) p->bit_rate = bitrate;
        if (major == MFMediaType_Video) {
            p->codec_type = AVMEDIA_TYPE_VIDEO;
            p->codec_id = subtype == MFVideoFormat_H264 ? AV_CODEC_ID_H264 :
                subtype == MFVideoFormat_HEVC ? AV_CODEC_ID_HEVC : AV_CODEC_ID_AV1;
            UINT32 width = 0, height = 0, n = 0, d = 0;
            MFGetAttributeSize(s.type.Get(), MF_MT_FRAME_SIZE, &width, &height);
            MFGetAttributeRatio(s.type.Get(), MF_MT_FRAME_RATE, &n, &d);
            p->width = static_cast<int>(width); p->height = static_cast<int>(height);
            // Some hardware MFTs advertise block-aligned storage dimensions.
            // Preserve the visible aperture without modifying the coded SPS.
            for (const auto& key : {MF_MT_MINIMUM_DISPLAY_APERTURE, MF_MT_GEOMETRIC_APERTURE}) {
                MFVideoArea area{}; UINT32 size = 0;
                if (SUCCEEDED(s.type->GetBlob(key, reinterpret_cast<UINT8*>(&area), sizeof(area), &size)) &&
                    size == sizeof(area) && area.OffsetX.value >= 0 && area.OffsetY.value >= 0 &&
                    area.Area.cx > 0 && area.Area.cy > 0 &&
                    static_cast<uint64_t>(area.OffsetX.value) + area.Area.cx <= width &&
                    static_cast<uint64_t>(area.OffsetY.value) + area.Area.cy <= height) {
                    p->width = area.Area.cx; p->height = area.Area.cy; break;
                }
            }
            s.output->avg_frame_rate = {static_cast<int>(n), static_cast<int>(d)};
            s.output->time_base = kMfTimeBase;
            UINT32 sn = 1, sd = 1;
            MFGetAttributeRatio(s.type.Get(), MF_MT_PIXEL_ASPECT_RATIO, &sn, &sd);
            if (sn > INT_MAX || sd > INT_MAX) return MF_E_INVALIDMEDIATYPE;
            p->sample_aspect_ratio = {static_cast<int>(sn ? sn : 1), static_cast<int>(sd ? sd : 1)};
            UINT32 range = 0, matrix = 0;
            if (SUCCEEDED(s.type->GetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, &range))) {
                if (range == MFNominalRange_0_255) p->color_range = AVCOL_RANGE_JPEG;
                else if (range == MFNominalRange_16_235) p->color_range = AVCOL_RANGE_MPEG;
            }
            if (SUCCEEDED(s.type->GetUINT32(MF_MT_YUV_MATRIX, &matrix))) {
                if (matrix == MFVideoTransferMatrix_BT709) p->color_space = AVCOL_SPC_BT709;
                else if (matrix == MFVideoTransferMatrix_BT601) p->color_space = AVCOL_SPC_SMPTE170M;
            }
            UINT32 primaries = 0, transfer = 0;
            if (SUCCEEDED(s.type->GetUINT32(MF_MT_VIDEO_PRIMARIES, &primaries))) {
                if (primaries == MFVideoPrimaries_BT709) p->color_primaries = AVCOL_PRI_BT709;
                else if (primaries == MFVideoPrimaries_SMPTE170M) p->color_primaries = AVCOL_PRI_SMPTE170M;
                else if (primaries == MFVideoPrimaries_SMPTE240M) p->color_primaries = AVCOL_PRI_SMPTE240M;
                else if (primaries == MFVideoPrimaries_BT470_2_SysBG) p->color_primaries = AVCOL_PRI_BT470BG;
                else if (primaries == MFVideoPrimaries_BT2020) p->color_primaries = AVCOL_PRI_BT2020;
            }
            if (SUCCEEDED(s.type->GetUINT32(MF_MT_TRANSFER_FUNCTION, &transfer))) {
                if (transfer == MFVideoTransFunc_709) p->color_trc = AVCOL_TRC_BT709;
                else if (transfer == MFVideoTransFunc_sRGB) p->color_trc = AVCOL_TRC_IEC61966_2_1;
                else if (transfer == MFVideoTransFunc_240M) p->color_trc = AVCOL_TRC_SMPTE240M;
                else if (transfer == MFVideoTransFunc_2084) p->color_trc = AVCOL_TRC_SMPTE2084;
                else if (transfer == MFVideoTransFunc_HLG) p->color_trc = AVCOL_TRC_ARIB_STD_B67;
            }
            hr = TypeExtra(s.type.Get(), MF_MT_MPEG_SEQUENCE_HEADER, p);
            if (FAILED(hr)) return hr;
            if(!p->extradata_size&&!s.codecHeader.empty()){
                if(FAILED(hr=SetExtra(p,s.codecHeader.data(),s.codecHeader.size())))return hr;
            }
            if (!p->extradata_size) {
                const AVBitStreamFilter* filter = av_bsf_get_by_name("extract_extradata");
                if (!filter) return MF_E_TOPO_CODEC_NOT_FOUND;
                result = av_bsf_alloc(filter, &s.extractor);
                if (result < 0) return Error(result);
                result = avcodec_parameters_copy(s.extractor->par_in, p);
                if (result < 0) return Error(result);
                s.extractor->time_base_in = kMfTimeBase;
                result = av_bsf_init(s.extractor);
                if (result < 0) return Error(result);
            }
        } else {
            p->codec_type = AVMEDIA_TYPE_AUDIO; p->codec_id = AV_CODEC_ID_AAC;
            UINT32 rate = 0, channels = 0, bytesPerSecond = 0;
            s.type->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &rate);
            s.type->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &channels);
            p->sample_rate = static_cast<int>(rate);
            av_channel_layout_default(&p->ch_layout, static_cast<int>(channels));
            p->frame_size = 1024;
            s.output->time_base = {1, static_cast<int>(rate)};
            if (SUCCEEDED(s.type->GetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, &bytesPerSecond)))
                p->bit_rate = static_cast<int64_t>(bytesPerSecond) * 8;
            // MF's AAC-LC encoder stores a 12-byte HEAACWAVEINFO tail before ASC.
            hr = TypeExtra(s.type.Get(), MF_MT_USER_DATA, p, 12);
            if (FAILED(hr)) return hr;
            if (!p->extradata_size) {
                const unsigned index = rate == 48000 ? 3 : 4;
                const uint8_t asc[]{static_cast<uint8_t>((2 << 3) | (index >> 1)),
                    static_cast<uint8_t>(((index & 1) << 7) | (channels << 3))};
                hr = SetExtra(p, asc, sizeof(asc));
                if (FAILED(hr)) return hr;
            }
        }
    }
    return S_OK;
}
HRESULT State::StartHeader() {
    if (header) return S_OK;
    if (!format) return S_FALSE;
    for (auto& s : streams) if (!s->seen || !s->output->codecpar->extradata_size) return S_FALSE;
    AVDictionary* options = nullptr;
    // Missing fragmentation options must fail closed, including allocation
    // failures: never silently produce a normal, unprotected MP4 instead.
    // Preserve the encoder's actual PTS/DTS, including reordered pictures.
    // negative_cts_offsets would additionally shift DTS by the first CTS delta
    // and can double the starting delay when paired with the edit list.
    // Matrix/range can be known while primaries/transfer remain unspecified.
    // The pinned muxer otherwise omits colr unless all three are specified.
    // Keep fragments recoverable during capture. At trailer time the pinned
    // muxer appends the full sample index, then hides the old fragment headers
    // inside mdat. No media readback, payload rewrite or re-encoding is needed.
    // The final index is written before committing the new mdat header; a
    // failed index write must not replace the recoverable fragmented prefix.
    const char* flags = videoAttributes
        ? "hybrid_fragmented+delay_moov+default_base_moof+frag_keyframe+write_colr"
        : "hybrid_fragmented+delay_moov+default_base_moof+frag_keyframe";
    int configured = av_dict_set(&options, "movflags", flags, 0);
    if (configured >= 0) configured = av_dict_set(&options, "frag_duration", "1000000", 0);
    if (configured >= 0) configured = av_dict_set(&options, "min_frag_duration", "1000000", 0);
    if (configured >= 0) configured = av_dict_set(&options, "video_track_timescale", "10000000", 0);
    if (configured >= 0) configured = av_dict_set(&options, "movie_timescale", "10000000", 0);
    if (configured >= 0) configured = av_dict_set(&options, "use_editlist", "1", 0);
    if (configured < 0) { av_dict_free(&options); return Error(configured); }
    const int result = avformat_write_header(format, &options);
    const bool unused = av_dict_count(options) != 0;
    av_dict_free(&options);
    if (result < 0) return Error(result);
    if (unused) return E_INVALIDARG;
    header = true;
    auto ready=std::move(pending);pending.clear();pendingBytes=0;
    for (auto& packet : ready) {
        HRESULT hr = WritePacket(std::move(packet));
        if (FAILED(hr)) return hr;
    }
    pending.clear(); pendingBytes = 0;
    return S_OK;
}
HRESULT State::WritePacket(Packet packet) {
    // Encoded MF samples can round their nominal frame duration differently
    // from adjacent decode timestamps. Retain one compressed video packet so
    // a fragment's last duration is exact; never let the muxer invent/drop a
    // timestamp when the next fragment begins one 100-ns tick earlier.
    auto& stream = *streams[packet->stream_index];
    if (stream.output->codecpar->codec_type != AVMEDIA_TYPE_VIDEO) return MuxPacket(std::move(packet));
    if (stream.pendingVideo) {
        const auto previous = stream.pendingVideo->dts;
        if (previous < 0 && packet->dts > INT64_MAX + previous) return MF_E_INVALID_TIMESTAMP;
        const int64_t duration = packet->dts - previous;
        if (duration <= 0) return MF_E_INVALID_TIMESTAMP;
        stream.pendingVideo->duration = duration;
        const HRESULT hr = MuxPacket(std::move(stream.pendingVideo));
        if (FAILED(hr)) return hr;
    }
    stream.pendingVideo = std::move(packet);
    return S_OK;
}
HRESULT State::MuxPacket(Packet packet) {
    if(split.basePath.empty())return MuxRaw(std::move(packet));
    const auto size=static_cast<size_t>(packet->size);
    if(queuedBytes+size>kMaxPendingBytes || videoPackets.size()+audioPackets.size()>=kMaxPendingPackets)
        return HRESULT_FROM_WIN32(ERROR_BUFFER_OVERFLOW);
    queuedBytes+=size;
    const bool video=format->streams[packet->stream_index]->codecpar->codec_type==AVMEDIA_TYPE_VIDEO;
    (video?videoPackets:audioPackets).push_back(std::move(packet));
    return DrainSegments(false);
}
HRESULT State::DrainSegments(bool finish) {
    auto pop=[&](std::deque<Packet>& queue){
        auto packet=std::move(queue.front());queue.pop_front();queuedBytes-=packet->size;return packet;
    };
    while(!videoPackets.empty()){
        const auto& video=videoPackets.front();
        bool roll=false;
        const bool safeKey=(video->flags&AV_PKT_FLAG_KEY) &&
            (format->streams[video->stream_index]->codecpar->codec_id!=AV_CODEC_ID_HEVC ||
             video->pts==video->dts || HasHevcIdr(video->data,video->size));
        if(SUCCEEDED(failure) && segmentHasVideo && safeKey && video->pts>segmentOrigin){
            QWORD length=0;
            if(split.bytes){const auto hr=bytes->GetLength(&length);if(FAILED(hr))return hr;}
            roll=(split.duration100ns&&uint64_t(video->pts-segmentOrigin)>=split.duration100ns) ||
                 (split.bytes&&length>=split.bytes);
        }
        if(roll && video->pts>video->dts){
            // An IDR_W_RADL may still carry leading display pictures. Wait only
            // through its reorder window, then refuse a cut that would trim one.
            if(!finish && videoPackets.back()->dts<video->pts)break;
            for(const auto& candidate:videoPackets){
                if(candidate->pts<video->pts){roll=false;break;}
                if(candidate->dts>=video->pts)break;
            }
        }
        const auto boundary=roll?video->pts:video->dts;
        // Hold audio until the next video decode boundary is known; when
        // rolling, wait through the keyframe's presentation time as well.
        // This keeps late/reordered audio in its original segment.
        if(!finish&&(audioPackets.empty()||audioPackets.back()->pts<boundary))break;
        if(finish && (audioPackets.empty()||audioPackets.back()->pts<boundary))roll=false;
        while(!audioPackets.empty()&&audioPackets.front()->pts<boundary){
            const auto hr=MuxRaw(pop(audioPackets));if(FAILED(hr))return hr;
        }
        if(roll){const auto hr=RollSegment(video->pts);if(FAILED(hr))return hr;}
        const auto hr=MuxRaw(pop(videoPackets));if(FAILED(hr))return hr;
        segmentHasVideo=true;
    }
    if(finish)while(!audioPackets.empty()){
        const auto hr=MuxRaw(pop(audioPackets));if(FAILED(hr))return hr;
    }
    return S_OK;
}
HRESULT State::RollSegment(int64_t origin) {
    // The encoder stays alive. Only the archive changes, at a clean video
    // boundary, with each compressed packet assigned to exactly one file.
    // Check the next destination before sealing the current file. A collision
    // or creation failure must not destroy the current playable recording.
    ComPtr<IMFByteStream> next;
    const auto path=RecordingPartPath(split.basePath,part+1);
    HRESULT hr=MFCreateFile(MF_ACCESSMODE_READWRITE,MF_OPENMODE_FAIL_IF_EXIST,MF_FILEFLAGS_NONE,path.c_str(),&next);
    if(FAILED(hr))return hr;
    ComPtr<IMFByteStream> buffered;
    if(FAILED(hr=BufferRecordingStorage(next.Get(),bytes.Get(),&buffered)))return hr;
    next=std::move(buffered);
    trailerWritten=true;
    hr=Error(av_write_trailer(format));if(FAILED(hr))return hr;
    avio_flush(io);if(FAILED(hr=Error(io->error)))return hr;
    if(FAILED(hr=RetireRecordingStorage(bytes.Get())))return hr;
    for(auto& s:streams){
        const auto* p=s->output->codecpar;
        if(p->extradata_size)s->codecHeader.assign(p->extradata,p->extradata+p->extradata_size);
        s->output=nullptr;
    }
    format->pb=nullptr;avformat_free_context(format);format=nullptr;
    av_freep(&io->buffer);avio_context_free(&io);
    bytes=std::move(next);++part;segmentOrigin=origin;segmentHasVideo=false;header=false;trailerWritten=false;
    if(FAILED(hr=Initialize()))return hr;
    return StartHeader();
}
HRESULT State::MuxRaw(Packet packet) {
    if(trailerWritten)return MF_E_INVALIDREQUEST;
    auto* stream = format->streams[packet->stream_index];
    packet->pts-=segmentOrigin;packet->dts-=segmentOrigin;
    av_packet_rescale_ts(packet.get(), kMfTimeBase, stream->time_base);
    const HRESULT hr = Error(av_interleaved_write_frame(format, packet.get()));
    if (FAILED(hr)) return hr;
    // Flush the small AVIO cache, not the OS disk cache, after each packet.
    // Completed fragments enter the bounded storage queue immediately. Its
    // coalesced flushes persist bytes independently of encoding and playback.
    avio_flush(io);
    return Error(io->error);
}
HRESULT State::Process(DWORD id, IMFSample* sample) {
    if (!sample) return E_POINTER;
    if (shutdown) return MF_E_SHUTDOWN;
    if (finalizing || finalized) return MF_E_INVALIDREQUEST;
    if (FAILED(failure)) return failure;
    auto* stream = Find(id);
    if (!stream) return MF_E_INVALIDSTREAMNUMBER;
    HRESULT hr = Initialize();
    if (FAILED(hr)) return failure = hr;
    LONGLONG time = 0, duration = 0;
    if (FAILED(hr = sample->GetSampleTime(&time)) || FAILED(hr = sample->GetSampleDuration(&duration))) return failure = hr;
    if (duration <= 0) return failure = MF_E_INVALID_TIMESTAMP;
    UINT64 decode = 0;
    const HRESULT decodeResult = sample->GetUINT64(MFSampleExtension_DecodeTimestamp, &decode);
    const int64_t dts = SUCCEEDED(decodeResult) ? static_cast<int64_t>(decode) : time;
    if (stream->lastDts != AV_NOPTS_VALUE && dts <= stream->lastDts) return failure = MF_E_INVALID_TIMESTAMP;
    ComPtr<IMFMediaBuffer> buffer;
    if (FAILED(hr = sample->ConvertToContiguousBuffer(&buffer))) return failure = hr;
    DWORD size = 0;
    if (FAILED(hr = buffer->GetCurrentLength(&size))) return failure = hr;
    if (!size || size > kMaxSampleBytes) return failure = MF_E_INVALID_STREAM_DATA;
    Packet packet(av_packet_alloc());
    if (!packet) return failure = E_OUTOFMEMORY;
    const int allocated = av_new_packet(packet.get(), static_cast<int>(size));
    if (allocated < 0) return failure = Error(allocated);
    BYTE* data = nullptr;
    if (FAILED(hr = buffer->Lock(&data, nullptr, nullptr))) return failure = hr;
    std::memcpy(packet->data, data, size);
    if (FAILED(hr = buffer->Unlock())) return failure = hr;
    packet->pts = time; packet->dts = dts; packet->duration = duration;
    packet->stream_index = stream->output->index;
    UINT32 key = 0;
    if (stream->output->codecpar->codec_type == AVMEDIA_TYPE_AUDIO ||
        (SUCCEEDED(sample->GetUINT32(MFSampleExtension_CleanPoint, &key)) && key)) packet->flags |= AV_PKT_FLAG_KEY;
    if (stream->extractor && !stream->output->codecpar->extradata_size) {
        int result = av_bsf_send_packet(stream->extractor, packet.get());
        if (result < 0) return failure = Error(result);
        result = av_bsf_receive_packet(stream->extractor, packet.get());
        if (result < 0) return failure = Error(result);
        size_t extraSize = 0;
        const auto* extra = av_packet_get_side_data(packet.get(), AV_PKT_DATA_NEW_EXTRADATA, &extraSize);
        if (extra && extraSize) {
            hr = SetExtra(stream->output->codecpar, extra, extraSize);
            if (FAILED(hr)) return failure = hr;
            av_bsf_free(&stream->extractor);
        }
    }
    stream->seen = true; stream->lastDts = dts;
    if (header) hr = WritePacket(std::move(packet));
    else {
        if (pending.size() >= kMaxPendingPackets || pendingBytes + size > kMaxPendingBytes)
            return failure = MF_E_INVALIDMEDIATYPE;
        pendingBytes += size;
        pending.push_back(std::move(packet));
        hr = StartHeader();
    }
    if (FAILED(hr)) failure = hr;
    return SUCCEEDED(hr) ? S_OK : hr;
}
HRESULT State::Finalize() {
    if (shutdown) return MF_E_SHUTDOWN;
    if (finalized) return MF_E_INVALIDREQUEST;
    if(trailerWritten){finalized=true;return FAILED(failure)?failure:MF_E_INVALIDREQUEST;}
    // A failed header may leave muxer-private initialization partially done.
    // Never retry that transaction. Already written headers can still drain.
    if (!header && FAILED(failure)) { finalized = true; return failure; }
    HRESULT hr = header ? S_OK : StartHeader();
    if (hr == S_FALSE) hr = MF_E_INVALID_STREAM_DATA;
    // Even after an input/encoder error, close every complete fragment possible.
    if (header) {
        for (auto& stream : streams) if (stream->pendingVideo) {
            const HRESULT pendingResult = MuxPacket(std::move(stream->pendingVideo));
            if (FAILED(pendingResult)) hr = pendingResult;
        }
        if(trailerWritten){finalized=true;return FAILED(hr)?hr:MF_E_INVALIDREQUEST;}
        if(!split.basePath.empty()){
            const auto drained=DrainSegments(true);if(FAILED(drained))hr=drained;
        }
        if(trailerWritten){finalized=true;return FAILED(hr)?hr:MF_E_INVALIDREQUEST;}
        trailerWritten=true;
        const HRESULT trailer = Error(av_write_trailer(format));
        if (FAILED(trailer)) hr = trailer;
        avio_flush(io);
        const HRESULT flushed = Error(io->error);
        if (FAILED(flushed)) hr = flushed;
    }
    if (SUCCEEDED(hr) && FAILED(failure)) hr = failure;
    finalized = true;
    return hr;
}

class Stream final : public RuntimeClass<RuntimeClassFlags<ClassicCom>,
    Microsoft::WRL::ChainInterfaces<IMFStreamSink, IMFMediaEventGenerator>, IMFMediaTypeHandler> {
public:
    std::shared_ptr<State> state;
    ComPtr<IMFMediaEventQueue> events;
    DWORD id = 0;
    HRESULT Initialize(std::shared_ptr<State> shared, DWORD identifier) {
        state = std::move(shared); id = identifier;
        return MFCreateEventQueue(&events);
    }
    HRESULT STDMETHODCALLTYPE GetEvent(DWORD flags, IMFMediaEvent** event) override { return events->GetEvent(flags, event); }
    HRESULT STDMETHODCALLTYPE BeginGetEvent(IMFAsyncCallback* callback, IUnknown* context) override { return events->BeginGetEvent(callback, context); }
    HRESULT STDMETHODCALLTYPE EndGetEvent(IMFAsyncResult* result, IMFMediaEvent** event) override { return events->EndGetEvent(result, event); }
    HRESULT STDMETHODCALLTYPE QueueEvent(MediaEventType type, REFGUID extended, HRESULT result, const PROPVARIANT* context) override {
        return events->QueueEventParamVar(type, extended, result, context);
    }
    HRESULT STDMETHODCALLTYPE GetMediaSink(IMFMediaSink** sink) override;
    HRESULT STDMETHODCALLTYPE GetIdentifier(DWORD* identifier) override {
        if (!identifier) return E_POINTER;
        std::lock_guard lock(state->mutex); if (state->shutdown) return MF_E_SHUTDOWN;
        *identifier = id; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetMediaTypeHandler(IMFMediaTypeHandler** handler) override {
        if (!handler) return E_POINTER; *handler = nullptr;
        std::lock_guard lock(state->mutex); if (state->shutdown) return MF_E_SHUTDOWN;
        return QueryInterface(IID_PPV_ARGS(handler));
    }
    HRESULT STDMETHODCALLTYPE ProcessSample(IMFSample* sample) override {
        HRESULT hr = S_OK;
        try { std::lock_guard lock(state->mutex); hr = state->Process(id, sample); }
        catch (...) { std::lock_guard lock(state->mutex); state->failure = hr = E_OUTOFMEMORY; }
        if (SUCCEEDED(hr)) return QueueEvent(MEStreamSinkRequestSample, GUID_NULL, S_OK, nullptr);
        QueueEvent(MEError, GUID_NULL, hr, nullptr);
        return hr;
    }
    HRESULT STDMETHODCALLTYPE PlaceMarker(MFSTREAMSINK_MARKER_TYPE, const PROPVARIANT*, const PROPVARIANT* context) override {
        std::lock_guard lock(state->mutex);
        if (state->shutdown) return MF_E_SHUTDOWN;
        // Samples are muxed synchronously, so this marker cannot overtake them.
        return QueueEvent(MEStreamSinkMarker, GUID_NULL, state->failure, context);
    }
    HRESULT STDMETHODCALLTYPE Flush() override {
        std::lock_guard lock(state->mutex);
        if (state->shutdown) return MF_E_SHUTDOWN;
        // There is no asynchronous application sample queue to flush. Packets
        // already accepted by the archive remain part of the saved recording.
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE IsMediaTypeSupported(IMFMediaType* type, IMFMediaType** closest) override {
        if (closest) *closest = nullptr;
        std::lock_guard lock(state->mutex); if (state->shutdown) return MF_E_SHUTDOWN;
        HRESULT hr = CheckType(type); if (FAILED(hr)) return hr;
        auto* info = state->Find(id); if (!info) return MF_E_INVALIDSTREAMNUMBER;
        if (info->type) {
            GUID a{}, b{}; info->type->GetGUID(MF_MT_SUBTYPE, &a); type->GetGUID(MF_MT_SUBTYPE, &b);
            if (a != b) return MF_E_INVALIDMEDIATYPE;
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetMediaTypeCount(DWORD* count) override {
        if (!count) return E_POINTER;
        std::lock_guard lock(state->mutex); if (state->shutdown) return MF_E_SHUTDOWN;
        auto* info = state->Find(id); if (!info) return MF_E_INVALIDSTREAMNUMBER;
        *count = info->type ? 1 : 0; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetMediaTypeByIndex(DWORD index, IMFMediaType** type) override {
        if (!type) return E_POINTER; *type = nullptr;
        if (index) return MF_E_NO_MORE_TYPES;
        return GetCurrentMediaType(type);
    }
    HRESULT STDMETHODCALLTYPE SetCurrentMediaType(IMFMediaType* type) override {
        std::lock_guard lock(state->mutex);
        HRESULT hr = IsMediaTypeSupported(type, nullptr); if (FAILED(hr)) return hr;
        auto* info = state->Find(id);
        ComPtr<IMFMediaType> negotiated;
        if (FAILED(hr = CopyType(type, &negotiated, state->videoAttributes.Get()))) return hr;
        if (state->format) {
            DWORD flags = 0;
            return info->type->IsEqual(negotiated.Get(), &flags) == S_OK ? S_OK : MF_E_INVALIDREQUEST;
        }
        info->type = std::move(negotiated);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetCurrentMediaType(IMFMediaType** type) override {
        if (!type) return E_POINTER; *type = nullptr;
        std::lock_guard lock(state->mutex); if (state->shutdown) return MF_E_SHUTDOWN;
        auto* info = state->Find(id); if (!info) return MF_E_INVALIDSTREAMNUMBER;
        return info->type ? CopyType(info->type.Get(), type) : MF_E_NOT_INITIALIZED;
    }
    HRESULT STDMETHODCALLTYPE GetMajorType(GUID* major) override {
        if (!major) return E_POINTER;
        std::lock_guard lock(state->mutex); if (state->shutdown) return MF_E_SHUTDOWN;
        auto* info = state->Find(id); if (!info) return MF_E_INVALIDSTREAMNUMBER;
        return info->type ? info->type->GetGUID(MF_MT_MAJOR_TYPE, major) : MF_E_NOT_INITIALIZED;
    }
};

class FinalizeWork final : public RuntimeClass<RuntimeClassFlags<ClassicCom>, IMFAsyncCallback> {
public:
    std::shared_ptr<State> state;
    ComPtr<IMFAsyncResult> result;
    HRESULT STDMETHODCALLTYPE GetParameters(DWORD*, DWORD*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE Invoke(IMFAsyncResult*) override {
        HRESULT hr = S_OK;
        try { std::lock_guard lock(state->mutex); hr = state->Finalize(); }
        catch (...) { hr = E_OUTOFMEMORY; }
        result->SetStatus(hr);
        return MFInvokeCallback(result.Get());
    }
};

// Presentation clocks retain their state sinks. A relay references only the
// shared state, not the parent COM sink, so even a supplied clock cannot form a
// reference cycle that requires the client to remember an explicit Shutdown.
class ClockRelay final : public RuntimeClass<RuntimeClassFlags<ClassicCom>, IMFClockStateSink> {
    HRESULT Target(IMFClockStateSink** target);
public:
    std::shared_ptr<State> state;
    HRESULT STDMETHODCALLTYPE OnClockStart(MFTIME system, LONGLONG offset) override;
    HRESULT STDMETHODCALLTYPE OnClockRestart(MFTIME system) override;
    HRESULT STDMETHODCALLTYPE OnClockStop(MFTIME system) override;
    HRESULT STDMETHODCALLTYPE OnClockPause(MFTIME system) override;
    HRESULT STDMETHODCALLTYPE OnClockSetRate(MFTIME system, float rate) override;
};

class Sink final : public IMFFinalizableMediaSink, public IMFClockStateSink {
    std::atomic<ULONG> references_{1};
    std::shared_ptr<State> state_;
    std::vector<ComPtr<Stream>> streams_;
    ComPtr<IMFPresentationClock> clock_;
    ComPtr<ClockRelay> clockRelay_;
    ~Sink() { Shutdown(); }
    HRESULT ClockEvent(MediaEventType type, bool request) {
        std::lock_guard lock(state_->mutex); if (state_->shutdown) return MF_E_SHUTDOWN;
        for (auto& s : streams_) {
            HRESULT hr = s->QueueEvent(type, GUID_NULL, S_OK, nullptr);
            if (FAILED(hr)) return hr;
            if (request && FAILED(hr = s->QueueEvent(MEStreamSinkRequestSample, GUID_NULL, S_OK, nullptr))) return hr;
        }
        return S_OK;
    }
public:
    explicit Sink(IMFByteStream* bytes) : state_(std::make_shared<State>()) {
        state_->streams.reserve(2); streams_.reserve(2);
        state_->owner = this; state_->bytes = bytes;
    }
    void ConfigureSplit(const SplitPolicy& split,std::shared_ptr<RecordingArchive>* archive) {
        state_->split=split;if(archive)*archive=state_;
    }
    HRESULT ConfigureVideoAttributes(IMFAttributes* attributes) {
        if (!attributes) return S_OK;
        HRESULT hr=MFCreateAttributes(&state_->videoAttributes,4);
        return FAILED(hr)?hr:attributes->CopyAllItems(state_->videoAttributes.Get());
    }
    bool TryAddRef() {
        ULONG count = references_.load();
        while (count && !references_.compare_exchange_weak(count, count + 1)) {}
        return count != 0;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** value) override {
        if (!value) return E_POINTER; *value = nullptr;
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IMFMediaSink) || iid == __uuidof(IMFFinalizableMediaSink))
            *value = static_cast<IMFFinalizableMediaSink*>(this);
        else if (iid == __uuidof(IMFClockStateSink)) *value = static_cast<IMFClockStateSink*>(this);
        else return E_NOINTERFACE;
        AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override { const ULONG n = --references_; if (!n) delete this; return n; }
    HRESULT STDMETHODCALLTYPE GetCharacteristics(DWORD* value) override {
        if (!value) return E_POINTER;
        std::lock_guard lock(state_->mutex); if (state_->shutdown) return MF_E_SHUTDOWN;
        *value = MEDIASINK_RATELESS; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE AddStreamSink(DWORD id, IMFMediaType* type, IMFStreamSink** result) override {
        if (!result) return E_POINTER; *result = nullptr;
        try {
            std::lock_guard lock(state_->mutex); if (state_->shutdown) return MF_E_SHUTDOWN;
            if (state_->format || state_->finalizing) return MF_E_INVALIDREQUEST;
            if (state_->Find(id)) return MF_E_STREAMSINK_EXISTS;
            if (state_->streams.size() >= 2) return MF_E_INVALIDSTREAMNUMBER;
            if (type) {
                HRESULT hr = CheckType(type); if (FAILED(hr)) return hr;
                GUID major{}; type->GetGUID(MF_MT_MAJOR_TYPE, &major);
                for (auto& s : state_->streams) if (s->type) {
                    GUID existing{}; s->type->GetGUID(MF_MT_MAJOR_TYPE, &existing);
                    if (existing == major) return MF_E_INVALIDMEDIATYPE;
                }
            }
            auto info = std::make_unique<StreamInfo>(); info->id = id;
            HRESULT hr = type ? CopyType(type, &info->type, state_->videoAttributes.Get()) : S_OK; if (FAILED(hr)) return hr;
            auto stream = Make<Stream>(); if (!stream) return E_OUTOFMEMORY;
            hr = stream->Initialize(state_, id); if (FAILED(hr)) return hr;
            state_->streams.push_back(std::move(info)); streams_.push_back(stream);
            return stream.CopyTo(result);
        } catch (...) { return E_OUTOFMEMORY; }
    }
    HRESULT STDMETHODCALLTYPE RemoveStreamSink(DWORD id) override {
        std::lock_guard lock(state_->mutex); if (state_->shutdown) return MF_E_SHUTDOWN;
        if (state_->format || state_->finalizing) return MF_E_INVALIDREQUEST;
        for (size_t i = 0; i < streams_.size(); ++i) if (streams_[i]->id == id) {
            streams_[i]->events->Shutdown(); streams_.erase(streams_.begin() + i);
            state_->streams.erase(state_->streams.begin() + i); return S_OK;
        }
        return MF_E_INVALIDSTREAMNUMBER;
    }
    HRESULT STDMETHODCALLTYPE GetStreamSinkCount(DWORD* count) override {
        if (!count) return E_POINTER;
        std::lock_guard lock(state_->mutex); if (state_->shutdown) return MF_E_SHUTDOWN;
        *count = static_cast<DWORD>(streams_.size()); return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetStreamSinkByIndex(DWORD index, IMFStreamSink** result) override {
        if (!result) return E_POINTER; *result = nullptr;
        std::lock_guard lock(state_->mutex); if (state_->shutdown) return MF_E_SHUTDOWN;
        return index < streams_.size() ? streams_[index].CopyTo(result) : MF_E_INVALIDINDEX;
    }
    HRESULT STDMETHODCALLTYPE GetStreamSinkById(DWORD id, IMFStreamSink** result) override {
        if (!result) return E_POINTER; *result = nullptr;
        std::lock_guard lock(state_->mutex); if (state_->shutdown) return MF_E_SHUTDOWN;
        for (auto& s : streams_) if (s->id == id) return s.CopyTo(result);
        return MF_E_INVALIDSTREAMNUMBER;
    }
    HRESULT STDMETHODCALLTYPE SetPresentationClock(IMFPresentationClock* clock) override {
        std::lock_guard lock(state_->mutex); if (state_->shutdown) return MF_E_SHUTDOWN;
        if (clock_.Get() == clock) return S_OK;
        if (clock && !clockRelay_) {
            clockRelay_ = Make<ClockRelay>(); if (!clockRelay_) return E_OUTOFMEMORY;
            clockRelay_->state = state_;
        }
        HRESULT hr = clock ? clock->AddClockStateSink(clockRelay_.Get()) : S_OK;
        if (FAILED(hr)) return hr;
        if (clock_) clock_->RemoveClockStateSink(clockRelay_.Get());
        clock_ = clock; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetPresentationClock(IMFPresentationClock** clock) override {
        if (!clock) return E_POINTER; *clock = nullptr;
        std::lock_guard lock(state_->mutex); if (state_->shutdown) return MF_E_SHUTDOWN;
        return clock_ ? clock_.CopyTo(clock) : MF_E_NO_CLOCK;
    }
    HRESULT STDMETHODCALLTYPE Shutdown() override {
        ComPtr<IMFPresentationClock> clock;
        ComPtr<ClockRelay> relay;
        {
            std::lock_guard lock(state_->mutex);
            if (state_->shutdown) return MF_E_SHUTDOWN;
            state_->shutdown = true; state_->owner = nullptr;
            for (auto& s : streams_) s->events->Shutdown();
            streams_.clear(); clock.Swap(clock_); relay.Swap(clockRelay_);
            state_->ClearMux();
            // Do not Close: Recorder owns bytes and must Flush/read size later.
            if(state_->split.basePath.empty())state_->bytes.Reset();
        }
        if (clock) clock->RemoveClockStateSink(relay.Get());
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE BeginFinalize(IMFAsyncCallback* callback, IUnknown* user) override {
        if (!callback) return E_POINTER;
        auto work = Make<FinalizeWork>(); if (!work) return E_OUTOFMEMORY;
        HRESULT hr = MFCreateAsyncResult(nullptr, callback, user, &work->result);
        if (FAILED(hr)) return hr;
        {
            std::lock_guard lock(state_->mutex); if (state_->shutdown) return MF_E_SHUTDOWN;
            if (state_->finalizing) return MF_E_INVALIDREQUEST;
            state_->finalizing = true; work->state = state_;
        }
        hr = MFPutWorkItem(MFASYNC_CALLBACK_QUEUE_LONG_FUNCTION, work.Get(), nullptr);
        if (FAILED(hr)) { std::lock_guard lock(state_->mutex); state_->finalizing = false; }
        return hr;
    }
    HRESULT STDMETHODCALLTYPE EndFinalize(IMFAsyncResult* result) override {
        if (!result) return E_POINTER;
        std::lock_guard lock(state_->mutex); if (state_->shutdown) return MF_E_SHUTDOWN;
        return result->GetStatus();
    }
    HRESULT STDMETHODCALLTYPE OnClockStart(MFTIME, LONGLONG) override { return ClockEvent(MEStreamSinkStarted, true); }
    HRESULT STDMETHODCALLTYPE OnClockRestart(MFTIME) override { return ClockEvent(MEStreamSinkStarted, true); }
    HRESULT STDMETHODCALLTYPE OnClockStop(MFTIME) override { return ClockEvent(MEStreamSinkStopped, false); }
    HRESULT STDMETHODCALLTYPE OnClockPause(MFTIME) override { return ClockEvent(MEStreamSinkPaused, false); }
    HRESULT STDMETHODCALLTYPE OnClockSetRate(MFTIME, float) override { return ClockEvent(MEStreamSinkRateChanged, false); }
};
HRESULT ClockRelay::Target(IMFClockStateSink** target) {
    std::lock_guard lock(state->mutex);
    if (state->shutdown || !state->owner || !state->owner->TryAddRef()) return MF_E_SHUTDOWN;
    *target = static_cast<IMFClockStateSink*>(state->owner); return S_OK;
}
HRESULT ClockRelay::OnClockStart(MFTIME system, LONGLONG offset) {
    ComPtr<IMFClockStateSink> target; const HRESULT hr = Target(&target);
    return FAILED(hr) ? hr : target->OnClockStart(system, offset);
}
HRESULT ClockRelay::OnClockRestart(MFTIME system) {
    ComPtr<IMFClockStateSink> target; const HRESULT hr = Target(&target);
    return FAILED(hr) ? hr : target->OnClockRestart(system);
}
HRESULT ClockRelay::OnClockStop(MFTIME system) {
    ComPtr<IMFClockStateSink> target; const HRESULT hr = Target(&target);
    return FAILED(hr) ? hr : target->OnClockStop(system);
}
HRESULT ClockRelay::OnClockPause(MFTIME system) {
    ComPtr<IMFClockStateSink> target; const HRESULT hr = Target(&target);
    return FAILED(hr) ? hr : target->OnClockPause(system);
}
HRESULT ClockRelay::OnClockSetRate(MFTIME system, float rate) {
    ComPtr<IMFClockStateSink> target; const HRESULT hr = Target(&target);
    return FAILED(hr) ? hr : target->OnClockSetRate(system, rate);
}
HRESULT Stream::GetMediaSink(IMFMediaSink** sink) {
    if (!sink) return E_POINTER; *sink = nullptr;
    std::lock_guard lock(state->mutex);
    if (state->shutdown || !state->owner || !state->owner->TryAddRef()) return MF_E_SHUTDOWN;
    *sink = static_cast<IMFFinalizableMediaSink*>(state->owner); return S_OK;
}
}
HRESULT CreateFragmentedMp4Sink(IMFByteStream* bytes, IMFMediaSink** sink,
    const SplitPolicy* split,std::shared_ptr<RecordingArchive>* archive,IMFAttributes* videoAttributes) {
    if (!sink) return E_POINTER; *sink = nullptr;
    if (!bytes) return E_INVALIDARG;
    DWORD capabilities = 0;
    HRESULT hr = bytes->GetCapabilities(&capabilities);
    if (FAILED(hr)) return hr;
    if (!(capabilities & MFBYTESTREAM_IS_WRITABLE) || !(capabilities & MFBYTESTREAM_IS_SEEKABLE)) return E_INVALIDARG;
    try { ComPtr<Sink> created;created.Attach(new Sink(bytes));
        if(FAILED(hr=created->ConfigureVideoAttributes(videoAttributes)))return hr;
        if(split)created->ConfigureSplit(*split,archive);
        *sink=created.Detach();
        return S_OK; }
    catch (...) { return E_OUTOFMEMORY; }
}
}
