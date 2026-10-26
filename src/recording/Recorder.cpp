#include "recording/Recorder.h"
#include "recording/EncoderSupport.h"
#include "recording/RecordingContainer.h"
#include "recording/RecordingTimeline.h"
#include "recording/RecordingScaler.h"
#include "recording/RecordingPauseTimeline.h"
#include "recording/RecordingStorage.h"
#include "recording/RecordingAudioGain.h"
#include "recording/RecordingAdmission.h"
#include "recording/RecordingDeliveryPolicy.h"
#include "video/MjpegDecoder.h"
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <wrl/client.h>
#include <shlobj.h>
#include <filesystem>
#include <algorithm>
#include <cstring>
#include <cstdio>
#include <stdexcept>
#include <memory>
#include <limits>

namespace llcv::recording {
using Microsoft::WRL::ComPtr;
Recorder recorder;
namespace {
constexpr DWORD kAudioBufferBytes=19200; // 100 ms of 48 kHz PCM16 stereo.
void Check(HRESULT hr) { if (FAILED(hr)) throw hr; }
ComPtr<IMFMediaType> Type() {
    ComPtr<IMFMediaType> t; Check(MFCreateMediaType(&t)); return t;
}
void AudioType(IMFMediaType* t, bool encoded,int kbps) {
    Check(t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio));
    Check(t->SetGUID(MF_MT_SUBTYPE, encoded ? MFAudioFormat_AAC : MFAudioFormat_PCM));
    Check(t->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2));
    Check(t->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, 48000));
    Check(t->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16));
    Check(t->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, encoded ? kbps*1000/8 : 192000));
    Check(t->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, encoded ? 1 : 4));
    if (encoded) Check(t->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 0));
}
}
Recorder::~Recorder() { Shutdown();if(wake_)CloseHandle(wake_); }
void Recorder::TrackWorkerActivity(WorkerActivity activity) noexcept {
    workerActivity_.store((GetTickCount64()<<8)|static_cast<unsigned>(activity),std::memory_order_relaxed);
}
void Recorder::ReportQueueFailure(bool video, LONGLONG incomingTime, const wchar_t* reason) noexcept {
    const auto& queue=video?videoQueue_:audioQueue_;
    const auto read=queue.read.load(std::memory_order_acquire);
    const auto write=queue.write.load(std::memory_order_relaxed);
    const auto activity=workerActivity_.load(std::memory_order_relaxed);
    constexpr const wchar_t* stages[]={L"starting",L"waiting",L"storage",L"video",L"audio",L"write-video",L"write-audio",L"finalizing"};
    const auto stage=activity&255u;
    const auto elapsed=GetTickCount64()-(activity>>8);
    const auto span=write!=read && !queue.packets.empty()?incomingTime-queue.packets[read%queue.packets.size()].time:0;
    fwprintf(stderr,L"[recording] queue diagnostic: stream=%s reason=%s used=%zu capacity=%zu arrival=%u span_us=%lld worker=%s stage_ms=%llu\n",
        video?L"video":L"audio",reason,write-read,queue.packets.size(),arrivalClock_?1u:0u,span/10,
        stage<std::size(stages)?stages[stage]:L"unknown",elapsed);
}
void Recorder::Configure(const video::VideoSampleFormat& v,
                         const capture_audio::Format& a,
                         video_color::Configuration c, bool arrivalClock,const Options& options,
                         const AM_MEDIA_TYPE* captureType,video_color::MatrixOverride mjpegColorOverride,
                         video_color::RangeOverride sdrRangeOverride) {
    std::lock_guard life(lifecycle_);
    if (busy_) return;
    video_=v; audio_=a; color_=c; arrivalClock_=arrivalClock; options_=options;
    mjpegColorOverride_=mjpegColorOverride;
    sdrRangeOverride_=sdrRangeOverride;
    compressedFormat_.clear();compressedFormatType_=GUID_NULL;
    if(v.format==settings::VideoPixelFormat::Mjpeg && captureType){
        if(captureType->cbFormat && !captureType->pbFormat){video_.valid=false;return;}
        try {
            if(captureType->cbFormat)compressedFormat_.assign(
                captureType->pbFormat,captureType->pbFormat+captureType->cbFormat);
            compressedFormatType_=captureType->formattype;
        } catch (...) {video_.valid=false;}
    }
}
void Recorder::Fail(HRESULT hr, Operation operation) noexcept {
    HRESULT ok=S_OK;
    if (result_.compare_exchange_strong(ok,hr)) {
        fwprintf(stderr,L"[recording] stopped: 0x%08X\n",static_cast<unsigned>(hr));
        Failure none=Failure::None;
        failure_.compare_exchange_strong(none,ClassifyFailure(hr,operation));
    }
    Stop();
}
bool Recorder::Start(const std::wstring& path,const Options* options) {
    std::lock_guard life(lifecycle_);
    if (busy_) return false;
    if (worker_.joinable()) worker_.join();
    if(options)options_=*options;
    ++session_;
    result_=S_OK;
    stop_=false;
    finalizeResult_=S_OK;failure_=Failure::None;phase_=Phase::Starting;
    recordingStartedMs_=0;recordingStoppedMs_=0;
    TrackWorkerActivity(WorkerActivity::Starting);
    pauseGeneration_=0;
    {
        std::lock_guard status(statusMutex_);snapshot_={};snapshot_.path=path;
        pausedSinceMs_=totalPausedMs_=0;
    }
    if(!wake_){Fail(E_OUTOFMEMORY);phase_=Phase::Failed;return false;}
    if(video_.format==settings::VideoPixelFormat::Uyvy) {
        // Reject before file creation, encoder startup or a recording worker.
        // Never feed UYVY bytes into the existing YUY2 recording path.
        Fail(UnsupportedUyvyRecording,Operation::Input);phase_=Phase::Failed;return false;
    }
    const bool nv12=video_.format==settings::VideoPixelFormat::Nv12;
    const bool yuy2=video_.format==settings::VideoPixelFormat::Yuy2;
    const bool mjpeg=video_.format==settings::VideoPixelFormat::Mjpeg;
    if (!Valid(options_) || !video_.valid || (!nv12 && !yuy2 && !mjpeg) || video_.color.hdr10() ||
        video_.width<=0 || video_.height<=0 || video_.width>3840 || video_.height>2160 ||
        video_.width%2 || video_.height%2 || video_.fps<=0 || video_.fps>240 ||
        (!mjpeg && video_.stride<static_cast<UINT32>(video_.width*(yuy2?2:1))) ||
        (video_.interlace & 1) || audio_.channels!=2 || audio_.containerBits!=16 ||
        audio_.validBits!=16 ||
        audio_.encoding!=capture_audio::Encoding::IntegerPcm || audio_.blockAlign!=4) {
        Fail(MF_E_INVALIDMEDIATYPE,Operation::Input);phase_=Phase::Failed;return false;
    }
    try {
        videoCount_=0; audioCount_=0;
        audioAnchor_=-1; audioFrames_=0;
        videoLimiter_={options_.maxFps && options_.maxFps<video_.fps?options_.maxFps:0,video_.fps};
        busy_=true;
        worker_=std::thread(&Recorder::Run,this,path);
        return true;
    } catch (...) { busy_=false;Fail(E_OUTOFMEMORY);phase_=Phase::Failed;return false; }
}
bool Recorder::SetPaused(bool paused) {
    std::lock_guard life(lifecycle_);
    if(!busy_ || stop_ || phase_!=Phase::Recording)return false;
    std::lock_guard status(statusMutex_);
    if(bool(pauseGeneration_.load()&1)==paused)return true;
    const auto now=GetTickCount64();
    if(paused)pausedSinceMs_=now;
    else {totalPausedMs_+=now-pausedSinceMs_;pausedSinceMs_=0;}
    ++pauseGeneration_;
    SetEvent(wake_);
    return true;
}
void Recorder::Stop() noexcept {
    stop_=true;
    BeginFinalizing(phase_);
    uint64_t zero=0;recordingStoppedMs_.compare_exchange_strong(zero,GetTickCount64());
    SetEvent(wake_);
}
void Recorder::Shutdown() {
    std::lock_guard life(lifecycle_);
    Stop();
    if (worker_.joinable()) worker_.join();
    video_={};
}
void Recorder::PushAudioChunks(const BYTE* data, long length, LONGLONG start, uint64_t pauseGeneration) {
    if(length%4){Fail(E_INVALIDARG,Operation::Input);return;}
    const LONGLONG duration=static_cast<LONGLONG>(length/4)*10000000/48000;
    if(start>(std::numeric_limits<LONGLONG>::max)()-duration){Fail(E_INVALIDARG,Operation::Input);return;}
    const size_t count=(static_cast<size_t>(length)+kAudioBufferBytes-1)/kAudioBufferBytes;
    auto& queue=audioQueue_;
    const size_t write=queue.write.load(std::memory_order_relaxed);
    const size_t used=write-queue.read.load(std::memory_order_acquire);
    if(used>queue.packets.size() || count>queue.packets.size()-used){
        fwprintf(stderr,L"[recording] audio delivery queue cannot fit %zu packet chunks\n",count);
        Fail(HRESULT_FROM_WIN32(ERROR_BUFFER_OVERFLOW),Operation::Queue);return;
    }
    // Reserve every lease before copying. Slots stay invisible to the worker
    // until one release-store publishes the entire input packet; failures must
    // never record an apparently successful prefix and silently drop its tail.
    struct Unpublished {
        Ring& queue;size_t write,count=0;bool published=false;
        ~Unpublished(){if(!published)for(size_t i=0;i<count;++i)queue.packets[(write+i)%queue.packets.size()].buffer.Reset();}
    } pending{queue,write};
    for(size_t i=0;i<count;++i){
        auto buffer=audioBuffers_.Acquire();
        if(!buffer){Fail(HRESULT_FROM_WIN32(ERROR_BUFFER_OVERFLOW),Operation::Queue);return;}
        queue.packets[(write+i)%queue.packets.size()].buffer=std::move(buffer);
        ++pending.count;
    }
    size_t offset=0;
    for(size_t i=0;i<count;++i){
        auto& packet=queue.packets[(write+i)%queue.packets.size()];
        const DWORD bytes=static_cast<DWORD>((std::min)(static_cast<size_t>(length)-offset,static_cast<size_t>(kAudioBufferBytes)));
        BYTE* target=nullptr;Check(packet.buffer->Lock(&target,nullptr,nullptr));
        std::memcpy(target,data+offset,bytes);
        Check(packet.buffer->Unlock());Check(packet.buffer->SetCurrentLength(bytes));
        packet.time=start+static_cast<LONGLONG>(offset/4)*10000000/48000;
        packet.duration=static_cast<LONGLONG>(bytes/4)*10000000/48000;
        packet.pauseGeneration=pauseGeneration;packet.video=false;
        offset+=bytes;
    }
    pending.published=true;
    queue.write.store(write+count,std::memory_order_release);SetEvent(wake_);
}
void Recorder::Push(bool video, IMediaSample* sample) noexcept {
    const auto session=session_.load(std::memory_order_acquire);
    if (!Recording()) return;
    struct Guard {
        Recorder& owner;
        Guard(Recorder& r):owner(r){++owner.producers_;}
        ~Guard(){if(--owner.producers_==0 && owner.stop_)SetEvent(owner.wake_);}
    } guard(*this);
    if (!Recording() || session!=session_.load(std::memory_order_acquire)) return;
    const auto pauseGeneration=pauseGeneration_.load(std::memory_order_acquire);
    LONGLONG start=0,end=0; BYTE* data=nullptr;
    const HRESULT timeHr=arrivalClock_?S_OK:(sample?sample->GetTime(&start,&end):E_POINTER);
    if (arrivalClock_) {
        start=std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count()/100;
        // The timestamp describes the start of the delivered packet/frame.
        // Both streams use one clock; never mix driver time and arrival time.
        const LONGLONG duration=video?10000000LL/video_.fps:
            (sample?static_cast<LONGLONG>(sample->GetActualDataLength()/4)*10000000/48000:0);
        end=start;start-=duration;
        if (!video && sample) {
            // Arrival jitter must not punch gaps into otherwise contiguous PCM.
            // Anchor once to the common clock, then advance by sample count.
            if (audioAnchor_<0) audioAnchor_=start;
            start=audioAnchor_+static_cast<LONGLONG>(audioFrames_)*10000000/48000;
            audioFrames_+=static_cast<uint64_t>((std::max)(0L,sample->GetActualDataLength())/4);
            end=start+duration;
        }
    }
    // Keep arrival-clock audio sample counts advancing while paused, but do
    // not copy, queue or encode paused input.
    if(pauseGeneration&1)return;
    if (!sample || FAILED(timeHr) || start<0 ||
        FAILED(sample->GetPointer(&data)) || !data) {
        fwprintf(stderr,L"[recording] %s invalid sample time hr=%08X time=%lld end=%lld\n",
            video?L"video":L"audio",static_cast<unsigned>(timeHr),start,end);
        Fail(E_INVALIDARG,Operation::Input); return;
    }
    const long length=sample->GetActualDataLength();
    if (length<=0 || length>sample->GetSize()) { Fail(E_INVALIDARG,Operation::Input); return; }
    if(video && !videoLimiter_.Accept(start))return;
    try {
        // Some capture endpoints deliver >100 ms at once. Split only that
        // uncommon case; normal callback storage and publication stay unchanged.
        if(!video && length>static_cast<long>(kAudioBufferBytes)){
            PushAudioChunks(data,length,start,pauseGeneration);return;
        }
        // Each DirectShow pin has one serialized producer. The worker alone
        // consumes these independent SPSC rings; audio/video never lock each other.
        auto& queue=video?videoQueue_:audioQueue_;
        const size_t write=queue.write.load(std::memory_order_relaxed);
        if (write-queue.read.load(std::memory_order_acquire)>=queue.packets.size()) {
            fwprintf(stderr,L"[recording] %s delivery queue full (%zu packets)\n",video?L"video":L"audio",queue.packets.size());
            ReportQueueFailure(video,start,L"delivery-full");
            Fail(HRESULT_FROM_WIN32(ERROR_BUFFER_OVERFLOW),Operation::Queue); return;
        }
        Packet& p=queue.packets[write%queue.packets.size()];
        auto buffer=(video?videoBuffers_:audioBuffers_).Acquire();
        if(!buffer){
            fwprintf(stderr,L"[recording] %s encoder buffer leases exhausted\n",video?L"video":L"audio");
            ReportQueueFailure(video,start,L"leases-exhausted");
            Fail(HRESULT_FROM_WIN32(ERROR_BUFFER_OVERFLOW),Operation::Queue);return;
        }
        p.time=start;
        p.pauseGeneration=pauseGeneration;
        size_t packedBytes=0;
        size_t row=0,rows=0;
        if (video) {
            const bool mjpeg=video_.format==settings::VideoPixelFormat::Mjpeg;
            const bool nv12=video_.format==settings::VideoPixelFormat::Nv12;
            row=static_cast<size_t>(video_.width)*(nv12?1:2);
            rows=static_cast<size_t>(video_.height)*(nv12?3:2)/2;
            if (!mjpeg && static_cast<size_t>(length)<video_.stride*rows) {
                fwprintf(stderr,L"[recording] short video %ld < %zu\n",length,video_.stride*rows);
                Fail(E_INVALIDARG,Operation::Input); return;
            }
            packedBytes=mjpeg?static_cast<size_t>(length):row*rows;
            p.duration=videoLimiter_.fps?10000000LL/videoLimiter_.fps:
                (end>start?end-start:10000000LL/video_.fps);
        } else {
            if (length%4) { Fail(E_INVALIDARG,Operation::Input); return; }
            packedBytes=static_cast<size_t>(length);
            p.duration=static_cast<LONGLONG>(length/4)*10000000/48000;
        }
        if(video && video_.format==settings::VideoPixelFormat::Mjpeg){
            DWORD capacity=0;Check(buffer->GetMaxLength(&capacity));
            if(packedBytes>capacity){Fail(MF_E_BUFFERTOOSMALL,Operation::Input);return;}
        }
        BYTE* target=nullptr;Check(buffer->Lock(&target,nullptr,nullptr));
        if(!video || video_.format==settings::VideoPixelFormat::Mjpeg || row==video_.stride)std::memcpy(target,data,packedBytes);
        else for(size_t y=0;y<rows;++y)std::memcpy(target+y*row,data+y*video_.stride,row);
        Check(buffer->Unlock());Check(buffer->SetCurrentLength(static_cast<DWORD>(packedBytes)));
        p.buffer=std::move(buffer);p.video=video;
        queue.write.store(write+1,std::memory_order_release); SetEvent(wake_);
    } catch (HRESULT hr) { Fail(hr);
    } catch (...) { Fail(E_OUTOFMEMORY); }
}
void Recorder::PrepareQueues() {
    const bool mjpeg=video_.format==settings::VideoPixelFormat::Mjpeg;
    const bool yuy2=video_.format==settings::VideoPixelFormat::Yuy2;
    const size_t frameBytes=static_cast<size_t>(video_.width)*video_.height*(yuy2?2:3)/(yuy2?1:2);
    const auto policy=RecordingDeliveryPolicy(mjpeg);
    videoQueue_.write=0;videoQueue_.read=0;audioQueue_.write=0;audioQueue_.read=0;
    videoQueue_.packets.resize(policy.videoPackets);audioQueue_.packets.resize(policy.audioPackets);
    videoBuffers_.Allocate(policy.videoBuffers,mjpeg
        ? (video_.bytes?video_.bytes:static_cast<DWORD>(video_.width*video_.height*3))
        : static_cast<DWORD>(frameBytes));
    audioBuffers_.Allocate(policy.audioBuffers,kAudioBufferBytes);
}
void Recorder::Run(std::wstring path) {
    const HRESULT com=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
    bool mf=false;
    ComPtr<IMFSinkWriter> writer;
    ComPtr<IMFByteStream> bytes;
    std::shared_ptr<RecordingArchive> archive;
    std::unique_ptr<RecordingTimeline> timeline;
    RecordingScaler scaler;
    video::MjpegDecoder mjpegDecoder;
    RecordingBufferPool decodedFrames;
    RecordingPauseTimeline pauseTimeline;
    bool begun=false;
    Operation operation=Operation::Storage;
    std::wstring folder;
    try {
        Check(com); Check(MFStartup(MF_VERSION,MFSTARTUP_LITE)); mf=true;
        std::error_code error;
        const auto absolute=std::filesystem::absolute(std::filesystem::path(path),error);
        if(error)Check(HRESULT_FROM_WIN32(static_cast<DWORD>(error.value())));
        folder=absolute.parent_path().wstring();
        std::filesystem::create_directories(folder,error);
        if(error)Check(HRESULT_FROM_WIN32(static_cast<DWORD>(error.value())));
        auto spaceAvailable=[&] {
            ULARGE_INTEGER free{};
            Check(GetDiskFreeSpaceExW(folder.c_str(),&free,nullptr,nullptr)?S_OK:HRESULT_FROM_WIN32(GetLastError()));
            {
                std::lock_guard status(statusMutex_);
                snapshot_.freeBytes=free.QuadPart;snapshot_.freeBytesKnown=true;
            }
            return free.QuadPart>=ReserveBytes(options_);
        };
        if(!spaceAvailable()) Check(HRESULT_FROM_WIN32(ERROR_DISK_FULL));
        // Allocate/free the bounded recording storage only on this worker.
        const bool yuy2=video_.format==settings::VideoPixelFormat::Yuy2;
        const bool mjpeg=video_.format==settings::VideoPixelFormat::Mjpeg;
        const size_t frameBytes=static_cast<size_t>(video_.width)*video_.height*(yuy2?2:3)/(yuy2?1:2);
        PrepareQueues();
        const auto dimensions=OutputSize(options_,video_.width,video_.height);
        const bool scaling=dimensions.width!=video_.width || dimensions.height!=video_.height;
        auto recordingColor=color_;
        if(mjpeg){
            operation=Operation::Input;
            AM_MEDIA_TYPE type{};type.formattype=compressedFormatType_;
            type.cbFormat=static_cast<ULONG>(compressedFormat_.size());type.pbFormat=compressedFormat_.data();
            Check(mjpegDecoder.initialize(video_.width,video_.height,video_.fps,
                &type,&video_.color,mjpegColorOverride_,nullptr,sdrRangeOverride_));
            recordingColor=mjpegDecoder.colorConfiguration();
            // Native recording needs leases that an asynchronous encoder may
            // retain. Downscaling already produces its own encoder-safe lease.
            if(!scaling)decodedFrames.Allocate(16,static_cast<DWORD>(frameBytes));
        }
        operation=Operation::Storage;
        scaler.Configure(!yuy2,video_.width,video_.height,dimensions.width,dimensions.height);
        const UINT32 matrix=recordingColor.matrix==video_color::Matrix::Bt709
            ? MFVideoTransferMatrix_BT709:MFVideoTransferMatrix_BT601;
        const UINT32 range=recordingColor.range==video_color::Range::Full
            ? MFNominalRange_0_255:MFNominalRange_16_235;
        ComPtr<IMFAttributes> attrs; Check(MFCreateAttributes(&attrs,2));
        Check(attrs->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS,options_.encoder==L"cpu"?FALSE:TRUE));
        Check(attrs->SetUINT32(MF_MT_YUV_MATRIX,matrix));
        Check(attrs->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE,range));
#ifdef LLCV_RECORDING_MODERN_MP4
        // The Windows H.264 sink can strip these attributes for raw inputs too.
        constexpr bool preserveColor=true;
#else
        // Playback-only test builds keep their dependency-free raw H.264 path;
        // distributed recording builds always include the packet-only muxer.
        const bool preserveColor=mjpeg;
#endif
        // Keep writer backpressure ON, but exclusively on this worker.
        Check(CreateRecordingFile(path.c_str(),attrs.Get(),&bytes,&writer,options_.codec,&options_,&archive,preserveColor));
        if(archive){std::lock_guard status(statusMutex_);snapshot_.path=archive->CurrentPath();}
        operation=Operation::Encoder;
        auto output=Type();
        Check(output->SetGUID(MF_MT_MAJOR_TYPE,MFMediaType_Video));
        Check(output->SetGUID(MF_MT_SUBTYPE,CodecSubtype(options_.codec)));
        Check(output->SetUINT32(MF_MT_AVG_BITRATE,options_.videoMbps*1000000));
        Check(output->SetUINT32(MF_MT_INTERLACE_MODE,MFVideoInterlace_Progressive));
        Check(MFSetAttributeSize(output.Get(),MF_MT_FRAME_SIZE,dimensions.width,dimensions.height));
        Check(MFSetAttributeRatio(output.Get(),MF_MT_FRAME_RATE,OutputFps(options_,video_.fps),1));
        Check(MFSetAttributeRatio(output.Get(),MF_MT_PIXEL_ASPECT_RATIO,1,1));
        Check(output->SetUINT32(MF_MT_YUV_MATRIX,matrix));
        Check(output->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE,range));
        DWORD videoStream=0,audioStream=0;
        Check(writer->AddStream(output.Get(),&videoStream));
        auto input=Type(); Check(output->CopyAllItems(input.Get()));
        Check(input->SetGUID(MF_MT_SUBTYPE,yuy2?MFVideoFormat_YUY2:MFVideoFormat_NV12));
        Check(ConfigureVideoInput(writer.Get(),videoStream,input.Get(),options_));
        auto audioOut=Type(); AudioType(audioOut.Get(),true,options_.audioKbps);
        Check(writer->AddStream(audioOut.Get(),&audioStream));
        auto audioIn=Type(); AudioType(audioIn.Get(),false,options_.audioKbps);
        Check(writer->SetInputMediaType(audioStream,audioIn.Get(),nullptr));
        timeline=std::make_unique<RecordingTimeline>(RequiresZeroOriginTimeline(options_.codec),videoStream,audioStream);
        Check(writer->BeginWriting()); begun=true;
        Check(VerifyRateControl(writer.Get(),videoStream,options_));
        recordingStartedMs_=GetTickCount64();
        if (!TryPublishRecording(phase_)) stop_=true;
        LONGLONG lastVideo=-1,lastAudio=-1;
        uint64_t nextSpaceCheck=GetTickCount64()+1000;
        for (;;) {
            operation=Operation::Storage;
            Check(RecordingStorageResult(bytes.Get()));
            if(!stop_ && GetTickCount64()>=nextSpaceCheck){
                operation=Operation::Storage;
                TrackWorkerActivity(WorkerActivity::Storage);
                // Queue durability work on the storage thread. Encoding keeps
                // consuming capture input during a slow disk flush.
                // Completed fragments survive abrupt process exit; device/OS
                // caches and the unfinished fragment still limit power-loss safety.
                Check(archive?archive->RequestFlush():RequestRecordingFlush(bytes.Get()));
                // Free-space metadata can block on filesystem/driver work too.
                // Reuse the storage worker; never hold up video/audio encoding.
                Check(RequestRecordingSpace(bytes.Get(),folder.c_str()));
                UpdateStorage(bytes.Get());
                bool lowSpace=false;
                {
                    std::lock_guard status(statusMutex_);
                    lowSpace=snapshot_.freeBytesKnown && snapshot_.freeBytes<ReserveBytes(options_);
                }
                if(lowSpace){
                    Failure none=Failure::None;failure_.compare_exchange_strong(none,Failure::DiskFull);
                    Stop();
                }
                nextSpaceCheck=GetTickCount64()+1000;
            }
            const size_t vr=videoQueue_.read.load(), ar=audioQueue_.read.load();
            const bool hasVideo=vr!=videoQueue_.write.load(std::memory_order_acquire);
            const bool hasAudio=ar!=audioQueue_.write.load(std::memory_order_acquire);
            if (!hasVideo && !hasAudio) {
                if (ReadyToFinalizeRecording(stop_,phase_,producers_,vr,videoQueue_.write,ar,audioQueue_.write)) break;
                TrackWorkerActivity(WorkerActivity::Waiting);
                const DWORD wait=WaitForSingleObject(wake_,1000);
                if(wait==WAIT_FAILED)Check(HRESULT_FROM_WIN32(GetLastError()));
                continue;
            }
            // A resumed epoch is anchored on its first video frame. Retain the
            // bounded PCM prefix until that shared cut is known.
            const bool resumeAudio=hasAudio&&pauseTimeline.NeedsVideo(audioQueue_.packets[ar%audioQueue_.packets.size()].pauseGeneration);
            if(resumeAudio&&!hasVideo&&!stop_){
                WaitForSingleObject(wake_,10);continue;
            }
            const bool takeVideo=hasVideo && (resumeAudio || !hasAudio ||
                videoQueue_.packets[vr%videoQueue_.packets.size()].time<=
                audioQueue_.packets[ar%audioQueue_.packets.size()].time);
            auto& queue=takeVideo?videoQueue_:audioQueue_;
            const size_t read=takeVideo?vr:ar;
            // Transfer the lease, not its pixels. In-flight encoding no longer
            // occupies a delivery slot; pool references still enforce the same
            // total memory bound until every asynchronous consumer releases it.
            Packet p=std::move(queue.packets[read%queue.packets.size()]);
            queue.read.store(read+1,std::memory_order_release);
            TrackWorkerActivity(p.video?WorkerActivity::Video:WorkerActivity::Audio);
            auto& last=p.video?lastVideo:lastAudio;
            const auto mapped=pauseTimeline.Map(p.video,p.pauseGeneration,p.time,p.duration);
            if (mapped.keep) {
                operation=Operation::Input;
                if (mapped.time<=last) Check(MF_E_INVALID_TIMESTAMP);
                last=mapped.time;
                // Gain runs off the capture/playback thread, before wrappers
                // or encoder leases can retain this recorder-owned buffer.
                if(!p.video && options_.audioVolumePercent!=100){
                    BYTE* pcm=nullptr;DWORD length=0;
                    Check(p.buffer->Lock(&pcm,nullptr,&length));
                    ApplyRecordingAudioGain(pcm,length,options_.audioVolumePercent);
                    Check(p.buffer->Unlock());
                }
                ComPtr<IMFSample> sample; Check(MFCreateSample(&sample));
                ComPtr<IMFMediaBuffer> buffer;
                if(p.video){
                    if(mjpeg){
                        ComPtr<IMFMediaBuffer> frame;
                        LONG frameStride=0;
                        BYTE* compressed=nullptr;DWORD length=0;
                        Check(p.buffer->Lock(&compressed,nullptr,&length));
                        ComPtr<IMFMediaBuffer> decoded;
                        const HRESULT decode=mjpegDecoder.decode(compressed,length,p.time,p.duration,&decoded,true);
                        const HRESULT unlocked=p.buffer->Unlock();
                        Check(decode);Check(unlocked);
                        const auto decodedColor=mjpegDecoder.colorConfiguration();
                        if(decodedColor.matrix!=recordingColor.matrix || decodedColor.range!=recordingColor.range)
                            Check(MF_E_INVALIDMEDIATYPE);
                        // A synchronous MJPEG frame must decode independently.
                        // Do not silently omit frames or move their timestamps.
                        if(!decoded)Check(MF_E_INVALID_STREAM_DATA);
                        if(scaling){
                            frame=std::move(decoded);
                            frameStride=mjpegDecoder.stride();
                        }else{
                            frame=decodedFrames.Acquire();
                            if(!frame)Check(HRESULT_FROM_WIN32(ERROR_BUFFER_OVERFLOW));
                            BYTE *source=nullptr,*target=nullptr;DWORD capacity=0,current=0;
                            Check(decoded->Lock(&source,&capacity,&current));
                            HRESULT copied=source && mjpegDecoder.validOutputBuffer(capacity,current)?S_OK:MF_E_INVALID_STREAM_DATA;
                            if(SUCCEEDED(copied))copied=frame->Lock(&target,nullptr,nullptr);
                            if(SUCCEEDED(copied)){
                                if(target){
                                    for(int row=0;row<video_.height*3/2;++row)
                                        std::memcpy(target+static_cast<size_t>(row)*video_.width,
                                            source+static_cast<size_t>(row)*mjpegDecoder.stride(),video_.width);
                                }else copied=E_POINTER;
                                const HRESULT unlockedFrame=frame->Unlock();
                                if(SUCCEEDED(copied))copied=unlockedFrame;
                            }
                            const HRESULT release=decoded->Unlock();Check(copied);Check(release);
                            Check(frame->SetCurrentLength(static_cast<DWORD>(frameBytes)));
                        }
                        Check(scaler.Apply(frame.Get(),&buffer,frameStride));
                    }else Check(scaler.Apply(p.buffer.Get(),&buffer));
                }
                else if(mapped.skipAudioFrames){
                    DWORD length=0;Check(p.buffer->GetCurrentLength(&length));
                    const DWORD skip=mapped.skipAudioFrames*4;
                    if(skip>=length)Check(MF_E_INVALID_STREAM_DATA);
                    Check(MFCreateMediaBufferWrapper(p.buffer.Get(),skip,length-skip,&buffer));
                }else buffer=p.buffer;
                Check(sample->AddBuffer(buffer.Get())); Check(sample->SetSampleTime(mapped.time));
                Check(sample->SetSampleDuration(mapped.duration));
                operation=Operation::Write;
                TrackWorkerActivity(p.video?WorkerActivity::WriteVideo:WorkerActivity::WriteAudio);
                Check(timeline->Push(writer.Get(),p.video,sample.Get()));
                (p.video?videoCount_:audioCount_).fetch_add(1);
            }
            p.buffer.Reset();
        }
    } catch (HRESULT hr) { Fail(hr,operation); }
      catch (...) { Fail(E_OUTOFMEMORY,operation); }
    Stop();
    // A callback that observed Phase::Recording may still own its ring slot.
    // Do not let Start/Configure recycle storage until it has left.
    while (producers_.load(std::memory_order_acquire)!=0) Sleep(1);
    if (begun) {
        TrackWorkerActivity(WorkerActivity::Finalizing);
        const HRESULT pending=timeline->Finish(writer.Get());
        if(FAILED(pending))Fail(pending,Operation::Write);
        const HRESULT hr=writer->Finalize();
        finalizeResult_=FinalizationResult(pending,hr);
        if (FAILED(hr)) Fail(hr,Operation::Finalize);
        if (!videoCount_ || !audioCount_) {
            finalizeResult_=MF_E_INVALID_STREAM_DATA;
            Fail(MF_E_INVALID_STREAM_DATA,Operation::Finalize);
        }
    }
    writer.Reset();
    timeline.reset();
    if(bytes) {
        const HRESULT flush=archive?archive->Flush():bytes->Flush();
        if(FAILED(flush)){finalizeResult_=flush;Fail(flush,Operation::Finalize);}
        if(archive)path=archive->CurrentPath();
        const HRESULT close=archive?archive->Close():bytes->Close();bytes.Reset();
        if(FAILED(close)){finalizeResult_=close;Fail(close,Operation::Finalize);}
        if(begun && videoCount_ && audioCount_) {
            const HRESULT verified=ValidateCompletedRecording(path.c_str());
            std::lock_guard status(statusMutex_);
            snapshot_.path=path;
            snapshot_.playable=SUCCEEDED(verified);
            if(FAILED(verified)){
                if(SUCCEEDED(finalizeResult_))finalizeResult_=verified;
                Fail(verified,Operation::Finalize);
            }
        }
    }
    std::vector<Packet>().swap(videoQueue_.packets);
    std::vector<Packet>().swap(audioQueue_.packets);
    videoBuffers_.Clear();audioBuffers_.Clear();
    scaler.Clear(); // Release MF-backed scaling buffers before the worker's MFShutdown.
    decodedFrames.Clear();mjpegDecoder.reset();
    archive.reset(); // Join storage after all split files close, before MFShutdown.
    if (mf) MFShutdown();
    if (SUCCEEDED(com)) CoUninitialize();
    phase_=SUCCEEDED(result_)?Phase::Saved:Phase::Failed;
    busy_=false;
}
Snapshot Recorder::GetSnapshot() const {
    Snapshot value;
    uint64_t paused=0,pausedSince=0;
    {
        std::lock_guard status(statusMutex_);value=snapshot_;
        paused=totalPausedMs_;pausedSince=pausedSinceMs_;
    }
    value.busy=Busy();value.phase=phase_.load();value.failure=failure_.load();
    value.result=Result();value.finalizeResult=finalizeResult_.load();
    const auto start=recordingStartedMs_.load(),stop=recordingStoppedMs_.load();
    const auto end=stop?stop:GetTickCount64();
    if(pausedSince && end>=pausedSince)paused+=end-pausedSince;
    value.elapsedMs=start && end>=start && end-start>=paused ? end-start-paused:0;
    value.paused=value.busy && value.phase==Phase::Recording && (pauseGeneration_.load()&1)!=0;
    return value;
}
void Recorder::UpdateStorage(IMFByteStream* stream) {
    const auto space=RecordingStorageSpace(stream);
    if(!space.completed)return; // Preserve the initial startup check while pending.
    std::lock_guard status(statusMutex_);
    snapshot_.freeBytesKnown=space.known;snapshot_.freeBytes=space.freeBytes;
}
} // namespace llcv::recording
