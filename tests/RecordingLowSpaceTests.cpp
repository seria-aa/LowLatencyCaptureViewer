#include "recording/Recorder.h"
#include "recording/RecordingStorage.h"
#include <mfapi.h>
#include <mfreadwrite.h>
#include <wrl/client.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <thread>

using Microsoft::WRL::ComPtr;
using namespace std::chrono_literals;
namespace {
int failures=0;
void Expect(bool condition,const char* message) {
    if(!condition){++failures;std::printf("FAIL: %s\n",message);}
}
void Check(HRESULT result) {if(FAILED(result))throw result;}
struct SpaceProbe {
    inline static SpaceProbe* current=nullptr;
    const DWORD producerThread=GetCurrentThreadId();
    std::atomic<bool> zero{false},wrongThread{false},invalidFolder{false};
    std::atomic<unsigned> unknownQueries{0},zeroQueries{0};
    static BOOL Query(const wchar_t* folder,ULARGE_INTEGER* free) {
        auto& probe=*current;
        if(GetCurrentThreadId()==probe.producerThread)probe.wrongThread=true;
        if(!folder || !*folder)probe.invalidFolder=true;
        free->QuadPart=0;
        if(probe.zero.load()){++probe.zeroQueries;return TRUE;}
        ++probe.unknownQueries;return FALSE;
    }
    SpaceProbe(){current=this;llcv::recording::SetRecordingSpaceQueryForTest(&Query);}
    ~SpaceProbe(){llcv::recording::SetRecordingSpaceQueryForTest(nullptr);current=nullptr;}
};
ComPtr<IMediaSample> Sample(IMemAllocator* allocator,long bytes,LONGLONG time,LONGLONG duration) {
    ComPtr<IMediaSample> sample;Check(allocator->GetBuffer(&sample,nullptr,nullptr,0));
    Check(sample->SetActualDataLength(bytes));LONGLONG end=time+duration;
    Check(sample->SetTime(&time,&end));return sample;
}
bool WaitReady(llcv::recording::Recorder& recorder) {
    const auto limit=std::chrono::steady_clock::now()+5s;
    while(recorder.Busy()&&!recorder.Recording()&&std::chrono::steady_clock::now()<limit)Sleep(1);
    return recorder.Recording();
}
bool WaitFinished(llcv::recording::Recorder& recorder) {
    const auto limit=std::chrono::steady_clock::now()+10s;
    while(recorder.Busy()&&std::chrono::steady_clock::now()<limit)Sleep(1);
    if(recorder.Busy()){
        // Do not turn a failed finalization test into an unbounded destructor
        // join. This is the dedicated child test process, never the viewer.
        std::puts("FAIL: recorder finalization exceeded 10 seconds");
        std::fflush(stdout);ExitProcess(1);
    }
    return true;
}
void VerifyDecoded(const wchar_t* path,bool video,size_t admittedPackets) {
    ComPtr<IMFSourceReader> reader;Check(MFCreateSourceReaderFromURL(path,nullptr,&reader));
    Check(reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS),FALSE));
    const DWORD stream=static_cast<DWORD>(video?MF_SOURCE_READER_FIRST_VIDEO_STREAM:MF_SOURCE_READER_FIRST_AUDIO_STREAM);
    Check(reader->SetStreamSelection(stream,TRUE));
    ComPtr<IMFMediaType> type;Check(MFCreateMediaType(&type));
    Check(type->SetGUID(MF_MT_MAJOR_TYPE,video?MFMediaType_Video:MFMediaType_Audio));
    Check(type->SetGUID(MF_MT_SUBTYPE,video?MFVideoFormat_NV12:MFAudioFormat_PCM));
    if(!video){
        Check(type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE,16));
        Check(type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS,2));
        Check(type->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND,48000));
    }
    Check(reader->SetCurrentMediaType(stream,nullptr,type.Get()));
    size_t samples=0;uint64_t audioFrames=0,audible=0;
    LONGLONG first=-1,last=-1,end=0,maxGap=0,maxVideoPtsError=0;bool eof=false;
    for(unsigned reads=0;reads<4096;++reads){
        DWORD flags=0;LONGLONG time=0;ComPtr<IMFSample> sample;
        Check(reader->ReadSample(stream,0,nullptr,&flags,&time,&sample));
        if(sample){
            Expect(time>last,"decoded timestamps strictly increase");
            if(!samples)first=time;
            else maxGap=(std::max)(maxGap,std::abs(time-end));
            last=time;
            ComPtr<IMFMediaBuffer> buffer;Check(sample->ConvertToContiguousBuffer(&buffer));
            BYTE* data=nullptr;DWORD bytes=0;Check(buffer->Lock(&data,nullptr,&bytes));
            if(video){
                Expect(data && bytes>=640u*360u*3u/2u,"every saved frame decodes to a complete NV12 image");
                const auto expected=static_cast<LONGLONG>(samples)*10000000/30;
                const auto error=std::abs(time-expected);
                maxVideoPtsError=(std::max)(maxVideoPtsError,error);
                Expect(time>=0 && (time*30+5000000)/10000000==static_cast<LONGLONG>(samples),
                    "saved video retains each expected frame ordinal");
                // The Windows MP4 reader was observed reconstructing both
                // native and decoded PTS one 1/30000 track tick (~33.3 us)
                // early, despite exact packet timestamps in the file. Allow
                // only 0.1 ms of reader quantization, NOT a 33.3 ms frame gap.
                Expect(error<=1000,"saved video PTS remains within 0.1 ms of the intended timeline");
                LONGLONG duration=0;Check(sample->GetSampleDuration(&duration));end=time+duration;
            }else{
                Expect(data && bytes && bytes%4==0,"saved audio decodes to complete stereo PCM frames");
                const auto* pcm=reinterpret_cast<const short*>(data);
                for(DWORD frame=0;frame<bytes/4;++frame)
                    if(std::abs(static_cast<int>(pcm[frame*2]))>100)++audible;
                audioFrames+=bytes/4;end=time+static_cast<LONGLONG>(bytes/4)*10000000/48000;
            }
            Check(buffer->Unlock());++samples;
        }
        if(flags&MF_SOURCE_READERF_ENDOFSTREAM){eof=true;break;}
    }
    Expect(eof && samples>0,"complete saved stream reaches end of file within bounded reads");
    Expect(first==0,"saved stream preserves its zero origin");
    if(video){
        std::printf("low-space saved video=%zu admitted=%zu max PTS error=%lld ticks (%.4fms)\n",
            samples,admittedPackets,maxVideoPtsError,maxVideoPtsError/10000.0);
        Expect(samples==admittedPackets,"automatic low-space stop drains every admitted video frame");
    }else{
        const uint64_t expected=static_cast<uint64_t>(admittedPackets)*480;
        std::printf("low-space decoded PCM=%llu expected=%llu max gap=%.3fms audible=%llu\n",
            audioFrames,expected,maxGap/10000.0,audible);
        Expect(audioFrames>=expected && audioFrames<=expected+4096,
            "automatic stop retains all admitted PCM within AAC framing padding");
        Expect(maxGap<=1000,"automatic stop leaves no audio timestamp holes");
        Expect(audible>expected/2,"saved AAC contains the supplied sound, not silent output");
    }
}
}
namespace llcv::recording {
struct RecorderTestAccess {
    static size_t Published(const Recorder& recorder,bool video) {
        return (video?recorder.videoQueue_:recorder.audioQueue_).write.load(std::memory_order_acquire);
    }
};
}
namespace {
void TestLowSpace() {
    using namespace llcv;
    SpaceProbe probe; // Outlives the recorder and its storage worker.
    recording::Recorder recorder;
    video::VideoSampleFormat video{};video.valid=true;video.width=video.stride=640;
    video.height=360;video.fps=30;video.format=settings::VideoPixelFormat::Nv12;
    capture_audio::Format audio{};audio.channels=2;audio.containerBits=audio.validBits=16;
    audio.blockAlign=4;audio.path=capture_audio::Path::Direct16BitStereo;
    recording::Options options;options.encoder=L"cpu";options.rateControl=recording::RateControl::Bitrate;
    options.maxFps=options.maxHeight=0;options.videoMbps=2;
    recorder.Configure(video,audio,{},false,options);
    const auto path=std::filesystem::temp_directory_path()/(L"LLCV-low-space-"+
        std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64())+L".mp4");
    Expect(recorder.Start(path.wstring()),"low-space recording starts with actual startup disk check");
    if(!WaitReady(recorder)){Expect(false,"CPU encoder ready within five seconds");recorder.Stop();WaitFinished(recorder);return;}
    Expect(recorder.GetSnapshot().freeBytesKnown,"actual startup capacity remains known before asynchronous refresh");
    ComPtr<IMemAllocator> allocator;
    Check(CoCreateInstance(CLSID_MemoryAllocator,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&allocator)));
    constexpr long frameBytes=640*360*3/2;
    ALLOCATOR_PROPERTIES requested{2,frameBytes,1,0},actual{};
    Check(allocator->SetProperties(&requested,&actual));Check(allocator->Commit());
    size_t acceptedVideo=0,acceptedAudio=0;unsigned nextVideo=0,rejectedAtStop=0;
    bool unknownObserved=false,zeroRequested=false;size_t framesWhenUnknown=0;
    const auto start=std::chrono::steady_clock::now();auto unknownSince=start;
    auto submit=[&](bool isVideo,IMediaSample* sample){
        const auto before=recording::RecorderTestAccess::Published(recorder,isVideo);
        recorder.Push(isVideo,sample);
        const auto after=recording::RecorderTestAccess::Published(recorder,isVideo);
        Expect(after==before || after==before+1,"one callback publishes at most one complete packet");
        if(after==before){
            ++rejectedAtStop;
            Expect(!recorder.Recording() && recorder.GetSnapshot().failure==recording::Failure::DiskFull,
                "only the automatic closed-admission boundary may reject a final callback");
        }else if(isVideo)++acceptedVideo;
        else ++acceptedAudio;
    };
    // Real-time 30 fps video and 10 ms stereo PCM, with no synthetic catch-up
    // burst added to hide or trigger a storage failure.
    for(unsigned tick=0;tick<800 && recorder.Recording();++tick){
        std::this_thread::sleep_until(start+std::chrono::milliseconds(tick*10));
        if(!recorder.Recording())break;
        const auto snapshot=recorder.GetSnapshot();
        if(!snapshot.freeBytesKnown && probe.unknownQueries.load() && !unknownObserved){
            unknownObserved=true;unknownSince=std::chrono::steady_clock::now();framesWhenUnknown=acceptedVideo;
        }
        if(unknownObserved && !zeroRequested && std::chrono::steady_clock::now()-unknownSince>=250ms){
            Expect(snapshot.phase==recording::Phase::Recording && snapshot.failure==recording::Failure::None,
                "unavailable periodic capacity does not stop or fail recording");
            Expect(acceptedVideo>=framesWhenUnknown+5,"video keeps progressing with unknown disk capacity");
            probe.zero=true;zeroRequested=true;
        }
        if(tick==nextVideo*100/30){
            auto sample=Sample(allocator.Get(),frameBytes,static_cast<LONGLONG>(nextVideo)*10000000/30,333333);
            BYTE* data=nullptr;Check(sample->GetPointer(&data));
            std::memset(data,64+nextVideo%128,640*360);std::memset(data+640*360,128,640*360/2);
            submit(true,sample.Get());++nextVideo;
        }
        if(recorder.Recording()){
            auto sample=Sample(allocator.Get(),480*4,static_cast<LONGLONG>(tick)*100000,100000);
            BYTE* data=nullptr;Check(sample->GetPointer(&data));auto* pcm=reinterpret_cast<short*>(data);
            for(unsigned frame=0;frame<480;++frame){
                const auto value=static_cast<short>(8000*std::sin((tick*480+frame)*6.283185307179586/48));
                pcm[frame*2]=pcm[frame*2+1]=value;
            }
            submit(false,sample.Get());
        }
    }
    Expect(unknownObserved && zeroRequested && probe.zeroQueries.load()>0,
        "recording observes unknown capacity followed by an asynchronous measured-zero result");
    Expect(!recorder.Recording(),"measured zero capacity automatically closes recording admission within eight seconds");
    if(recorder.Recording())recorder.Stop(); // Failure cleanup only; never the successful stop path.
    WaitFinished(recorder);recorder.Shutdown();Check(allocator->Decommit());
    const auto snapshot=recorder.GetSnapshot();
    Expect(snapshot.phase==recording::Phase::Saved && !snapshot.busy,"low-space stop completes normally");
    Expect(snapshot.failure==recording::Failure::DiskFull,"saved status preserves the low-space stop reason");
    Expect(SUCCEEDED(snapshot.result) && SUCCEEDED(snapshot.finalizeResult) && snapshot.playable,
        "low-space finalization succeeds and verifies the surviving MP4");
    Expect(snapshot.freeBytesKnown && snapshot.freeBytes==0,"recording status exposes the completed measured-zero result");
    Expect(!probe.wrongThread && !probe.invalidFolder,"periodic query runs off the producer using a valid destination");
    Expect(rejectedAtStop<=1,"only one in-flight callback can straddle automatic stop");
    Expect(acceptedVideo>0 && acceptedAudio>0,"fixture supplied both streams before automatic stop");
    VerifyDecoded(path.c_str(),true,acceptedVideo);VerifyDecoded(path.c_str(),false,acceptedAudio);
    std::printf("low-space integration unknown=%u zero=%u admitted video/audio=%zu/%zu stop-boundary rejection=%u\n",
        probe.unknownQueries.load(),probe.zeroQueries.load(),acceptedVideo,acceptedAudio,rejectedAtStop);
    std::printf("low-space recording: %ls\n",path.c_str());
}
}
int main() {
    const auto com=CoInitializeEx(nullptr,COINIT_MULTITHREADED);if(FAILED(com))return 1;
    const auto mf=MFStartup(MF_VERSION);if(FAILED(mf)){CoUninitialize();return 1;}
    try {TestLowSpace();}
    catch(HRESULT result){++failures;std::printf("HRESULT: 0x%08lX\n",static_cast<unsigned long>(result));}
    catch(...){++failures;std::puts("unexpected exception");}
    MFShutdown();CoUninitialize();return failures?1:0;
}
