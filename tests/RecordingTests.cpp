#include "recording/Recorder.h"
#include "recording/RecordingArchive.h"
#include "recording/EncoderSupport.h"
#include "recording/RecordingPauseTimeline.h"
#include "video/MjpegDecoder.h"
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>
#include <filesystem>
#include <cstdio>
#include <chrono>
#include <cmath>
#include <wincodec.h>
#include <cstring>
#include <array>
using Microsoft::WRL::ComPtr;
static int failures=0;
static void Expect(bool value,const char* message) { if(!value){++failures;printf("FAIL: %s\n",message);} }
static void Check(HRESULT hr) { if(FAILED(hr)) throw hr; }
namespace llcv::recording {
struct RecorderTestAccess {
    static void AudioChunkAtomicity() {
        constexpr long length=48004; // 250 ms plus one PCM stereo frame.
        std::vector<BYTE> source(length);
        for(size_t i=0;i<source.size();++i)source[i]=static_cast<BYTE>(i*31+7);
        for(size_t leases=0;leases<3;++leases){
            Recorder subject;
            subject.audioQueue_.packets.resize(4);subject.audioQueue_.read=7;subject.audioQueue_.write=7;
            subject.audioBuffers_.Allocate(leases,19200);
            subject.PushAudioChunks(source.data(),length,2500000,2);
            Expect(FAILED(subject.Result()),"mid-reservation lease exhaustion fails explicitly");
            Expect(subject.audioQueue_.write==7,"lease exhaustion never publishes a partial audio packet");
            for(const auto& packet:subject.audioQueue_.packets)Expect(!packet.buffer,"unpublished packet leases rolled back");
            std::array<ComPtr<IMFMediaBuffer>,3> returned;
            for(size_t i=0;i<leases;++i){returned[i]=subject.audioBuffers_.Acquire();Expect(bool(returned[i]),"every reserved lease returns after failure");}
            Expect(!subject.audioBuffers_.Acquire(),"rollback neither leaks nor creates extra leases");
        }
        Recorder subject;
        subject.audioQueue_.packets.resize(4);subject.audioQueue_.read=7;subject.audioQueue_.write=7;
        subject.audioBuffers_.Allocate(3,19200);
        subject.PushAudioChunks(source.data(),length,2500000,2);
        Expect(SUCCEEDED(subject.Result()) && subject.audioQueue_.write==10,"complete audio packet publishes all three wrapped slots");
        RecordingPauseTimeline timeline;
        timeline.Map(true,0,0,1000000);timeline.Map(false,0,0,1000000);
        const auto resumed=timeline.Map(true,2,3250000,200000);
        Expect(resumed.time==1000000,"resumed video defines a cut inside the original large callback");
        size_t offset=0;uint64_t keptFrames=0;
        for(size_t i=0;i<3;++i){
            const auto& packet=subject.audioQueue_.packets[(7+i)%4];
            BYTE* data=nullptr;DWORD bytes=0;Check(packet.buffer->Lock(&data,nullptr,&bytes));
            Expect(bytes==(std::min)(size_t(19200),source.size()-offset),"split chunks preserve exact PCM frame boundaries");
            Expect(std::memcmp(data,source.data()+offset,bytes)==0,"split chunks preserve every source byte in order");
            Check(packet.buffer->Unlock());
            Expect(packet.time==2500000+static_cast<LONGLONG>(offset/4)*10000000/48000 && packet.pauseGeneration==2,
                "split chunks retain source timestamps and one pause generation");
            const auto mapped=timeline.Map(false,packet.pauseGeneration,packet.time,packet.duration);
            Expect(mapped.keep && std::abs(mapped.time-(1000000+static_cast<LONGLONG>(keptFrames)*10000000/48000))<=1,
                "large callback spanning the resume cut maps without a gap or overlap");
            keptFrames+=bytes/4-mapped.skipAudioFrames;offset+=bytes;
        }
        Expect(offset==source.size() && keptFrames==8401,"resume trims only pre-cut PCM and retains every suffix frame");
    }
};
}
static std::vector<BYTE> JpegFrame(unsigned width,unsigned height,bool bright,bool textured=false,bool colored=false) {
    ComPtr<IWICImagingFactory> factory;
    Check(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&factory)));
    ComPtr<IStream> stream;Check(CreateStreamOnHGlobal(nullptr,TRUE,&stream));
    ComPtr<IWICBitmapEncoder> encoder;Check(factory->CreateEncoder(GUID_ContainerFormatJpeg,nullptr,&encoder));
    Check(encoder->Initialize(stream.Get(),WICBitmapEncoderNoCache));
    ComPtr<IWICBitmapFrameEncode> frame;Check(encoder->CreateNewFrame(&frame,nullptr));
    Check(frame->Initialize(nullptr));Check(frame->SetSize(width,height));
    WICPixelFormatGUID pixel=GUID_WICPixelFormat24bppBGR;Check(frame->SetPixelFormat(&pixel));
    Expect(pixel==GUID_WICPixelFormat24bppBGR,"JPEG fixture pixel format");
    std::vector<BYTE> pixels(static_cast<size_t>(width)*height*3,bright?235:16);
    if(textured)for(size_t i=0;i<pixels.size();++i)
        pixels[i]=static_cast<BYTE>(int(pixels[i])+int((i*2654435761u>>24)%17)-8);
    if(colored){
        // Large constant bars keep the comparison clear of JPEG/chroma and
        // resize boundaries: black, white, red, green, blue, yellow, cyan, magenta.
        constexpr BYTE bgr[][3]={{0,0,0},{255,255,255},{0,0,255},{0,255,0},
            {255,0,0},{0,255,255},{255,255,0},{255,0,255}};
        for(unsigned y=0;y<height;++y)for(unsigned x=0;x<width;++x)
            std::memcpy(pixels.data()+(static_cast<size_t>(y)*width+x)*3,bgr[x*8/width],3);
    }
    Check(frame->WritePixels(height,width*3,static_cast<UINT>(pixels.size()),pixels.data()));
    Check(frame->Commit());Check(encoder->Commit());
    STATSTG stat{};Check(stream->Stat(&stat,STATFLAG_NONAME));Check(stream->Seek({},STREAM_SEEK_SET,nullptr));
    std::vector<BYTE> jpeg(static_cast<size_t>(stat.cbSize.QuadPart));ULONG read=0;
    Check(stream->Read(jpeg.data(),static_cast<ULONG>(jpeg.size()),&read));
    Expect(read==jpeg.size(),"JPEG fixture bytes");return jpeg;
}
static ComPtr<IMediaSample> Sample(IMemAllocator* allocator, long bytes, LONGLONG time,LONGLONG duration) {
    ComPtr<IMediaSample> s; Check(allocator->GetBuffer(&s,nullptr,nullptr,0));
    Check(s->SetActualDataLength(bytes)); LONGLONG end=time+duration; Check(s->SetTime(&time,&end)); return s;
}
using ColorPatches=std::array<std::array<double,3>,8>;
static ColorPatches ReadColorPatches(const BYTE* data,UINT32 stride,UINT32 width,UINT32 height) {
    ColorPatches patches{};
    for(unsigned bar=0;bar<patches.size();++bar){
        const unsigned centerX=((bar*2+1)*width/16)&~1u,centerY=(height/2)&~1u;
        for(unsigned y=centerY-8;y<centerY+8;++y)for(unsigned x=centerX-8;x<centerX+8;++x){
            patches[bar][0]+=data[static_cast<size_t>(y)*stride+x]/256.0;
            const auto uv=static_cast<size_t>(height+y/2)*stride+(x&~1u);
            patches[bar][1]+=data[uv]/256.0;patches[bar][2]+=data[uv+1]/256.0;
        }
    }
    return patches;
}
static void TestRecordingColor(bool scale,llcv::settings::VideoPixelFormat format) {
    using namespace llcv;
    const bool mjpeg=format==settings::VideoPixelFormat::Mjpeg;
    const bool yuy2=format==settings::VideoPixelFormat::Yuy2;
    const char* input=mjpeg?"MJPEG":yuy2?"YUY2":"NV12";
    const unsigned width=scale?1280:640,height=scale?960:480;
    const unsigned outputWidth=scale?960:width,outputHeight=scale?720:height;
    const unsigned inputStride=mjpeg?0:width*(yuy2?2:1)+32;
    std::vector<BYTE> frame;
    ColorPatches reference{};
    if(mjpeg){
        frame=JpegFrame(width,height,false,false,true);
        // Compare with the actual decoded capture bytes, not ideal RGB values:
        // the override chooses interpretation; it does not recolor JPEG pixels.
        // This reference never passes through the recorder, scaler or encoder.
        video::MjpegDecoder decoder;
        Check(decoder.initialize(width,height,30,nullptr,nullptr,video_color::MatrixOverride::Auto,nullptr));
        ComPtr<IMFMediaBuffer> decoded;
        Check(decoder.decode(frame.data(),static_cast<DWORD>(frame.size()),0,333333,&decoded,true));
        Expect(decoded!=nullptr,"colored JPEG produces a reference frame");
        if(!decoded)return;
        BYTE* data=nullptr;DWORD capacity=0,length=0;Check(decoded->Lock(&data,&capacity,&length));
        Expect(data && decoder.validOutputBuffer(capacity,length),"colored JPEG reference layout");
        if(data && decoder.validOutputBuffer(capacity,length))
            reference=ReadColorPatches(data,decoder.stride(),width,height);
        Check(decoded->Unlock());
    }else{
        // Deliberately non-gray constant bars expose matrix/range conversion.
        // Row padding must not leak into either native or downscaled output.
        constexpr BYTE yuv[][3]={{16,128,128},{235,128,128},{81,90,240},{145,54,34},
            {41,240,110},{210,16,146},{170,166,16},{106,202,222}};
        frame.assign(static_cast<size_t>(inputStride)*(yuy2?height:height*3/2),0xff);
        for(unsigned bar=0;bar<reference.size();++bar)for(unsigned plane=0;plane<3;++plane)
            reference[bar][plane]=yuv[bar][plane];
        for(unsigned y=0;y<height;++y)for(unsigned x=0;x<width;x+=2){
            const auto& patch=yuv[x*8/width];
            if(yuy2){
                auto* packed=frame.data()+static_cast<size_t>(y)*inputStride+x*2;
                packed[0]=packed[2]=patch[0];packed[1]=patch[1];packed[3]=patch[2];
            }else{
                auto* row=frame.data()+static_cast<size_t>(y)*inputStride;
                row[x]=row[x+1]=patch[0];
                if(!(y&1)){
                    auto* uv=frame.data()+static_cast<size_t>(height+y/2)*inputStride+x;
                    uv[0]=patch[1];uv[1]=patch[2];
                }
            }
        }
    }
    struct Interpretation { video_color::MatrixOverride matrixOverride;video_color::RangeOverride rangeOverride;UINT32 matrix,range; };
    constexpr Interpretation interpretations[]={
        {video_color::MatrixOverride::Bt601,video_color::RangeOverride::Full,2,1},
        {video_color::MatrixOverride::Bt601,video_color::RangeOverride::Limited,2,2},
        {video_color::MatrixOverride::Bt709,video_color::RangeOverride::Full,1,1},
        {video_color::MatrixOverride::Bt709,video_color::RangeOverride::Limited,1,2}};
    for(const auto& interpretation:interpretations){
        video::VideoSampleFormat v{};v.valid=true;v.width=width;v.height=height;v.fps=30;
        v.format=format;v.bytes=static_cast<DWORD>(frame.size());v.stride=inputStride;
        capture_audio::Format a{};a.channels=2;a.containerBits=a.validBits=16;a.blockAlign=4;
        a.path=capture_audio::Path::Direct16BitStereo;
        recording::Options options;options.encoder=L"cpu";options.rateControl=recording::RateControl::Bitrate;
        options.maxHeight=scale?720:0;options.maxFps=0;
        // Match the viewer's resolver, with deliberately conflicting device
        // range metadata to verify the selected SDR range wins for raw input.
        const auto viewerColor=video_color::Resolve(mjpeg,width,height,{},
            {interpretation.matrix,interpretation.range==1?2u:1u},
            mjpeg?interpretation.matrixOverride:video_color::MatrixOverride::Auto,
            interpretation.rangeOverride);
        auto generic=viewerColor;
        // Opposite values expose accidentally reusing the generic capture color
        // instead of the MJPEG decoder's explicit override.
        if(mjpeg){
            generic.matrix=interpretation.matrix==1?video_color::Matrix::Bt601:video_color::Matrix::Bt709;
            generic.range=interpretation.range==1?video_color::Range::Limited:video_color::Range::Full;
        }
        recording::Recorder recorder;
        recorder.Configure(v,a,generic,false,options,nullptr,
            interpretation.matrixOverride,interpretation.rangeOverride);
        const auto path=std::filesystem::temp_directory_path()/(L"LLCV-recording-color-"+
            std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64())+L"-"+
            std::to_wstring(interpretation.matrix)+std::to_wstring(interpretation.range)+L".mp4");
        Expect(recorder.Start(path.wstring()),"colored recording starts");
        for(unsigned wait=0;wait<5000 && recorder.Busy()&&!recorder.Recording();++wait)Sleep(1);
        Expect(recorder.Recording(),"colored recording CPU encoder ready");
        if(!recorder.Recording()){recorder.Shutdown();continue;}
        ComPtr<IMemAllocator> allocator;
        Check(CoCreateInstance(CLSID_MemoryAllocator,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&allocator)));
        ALLOCATOR_PROPERTIES requested{2,static_cast<long>((std::max)(frame.size(),size_t{1920})),1,0},actual{};
        Check(allocator->SetProperties(&requested,&actual));Check(allocator->Commit());
        const auto start=std::chrono::steady_clock::now();unsigned frameNumber=0;
        for(unsigned tick=0;tick<40;++tick){
            std::this_thread::sleep_until(start+std::chrono::milliseconds(tick*10));
            if(frameNumber<12 && tick==frameNumber*100/30){
                auto sample=Sample(allocator.Get(),static_cast<long>(frame.size()),frameNumber*10000000LL/30,333333);
                BYTE* data=nullptr;Check(sample->GetPointer(&data));std::memcpy(data,frame.data(),frame.size());
                recorder.Push(true,sample.Get());++frameNumber;
            }
            auto sample=Sample(allocator.Get(),1920,tick*100000LL,100000);
            BYTE* data=nullptr;Check(sample->GetPointer(&data));std::memset(data,0,1920);
            recorder.Push(false,sample.Get());
        }
        recorder.Stop();recorder.Shutdown();Check(allocator->Decommit());
        Expect(SUCCEEDED(recorder.Result()),"colored recording finalizes");
        if(FAILED(recorder.Result()))continue;
        ComPtr<IMFSourceReader> reader;Check(MFCreateSourceReaderFromURL(path.c_str(),nullptr,&reader));
        const auto stream=static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
        Check(reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS),FALSE));
        Check(reader->SetStreamSelection(stream,TRUE));
        ComPtr<IMFMediaType> native;Check(reader->GetNativeMediaType(stream,0,&native));
        UINT32 matrix=0,range=0;native->GetUINT32(MF_MT_YUV_MATRIX,&matrix);native->GetUINT32(MF_MT_VIDEO_NOMINAL_RANGE,&range);
        printf("%s color %s requested matrix/range=%u/%u saved=%u/%u\n",input,scale?"scaled":"native",
            interpretation.matrix,interpretation.range,matrix,range);
        Expect(matrix==interpretation.matrix,"saved matrix matches the selected interpretation");
        Expect(range==interpretation.range,"saved range matches the selected interpretation");
        ComPtr<IMFMediaType> type;Check(MFCreateMediaType(&type));
        Check(type->SetGUID(MF_MT_MAJOR_TYPE,MFMediaType_Video));Check(type->SetGUID(MF_MT_SUBTYPE,MFVideoFormat_NV12));
        Check(reader->SetCurrentMediaType(stream,nullptr,type.Get()));
        ComPtr<IMFMediaType> current;Check(reader->GetCurrentMediaType(stream,&current));
        UINT32 savedWidth=0,savedHeight=0,stride=0;Check(MFGetAttributeSize(current.Get(),MF_MT_FRAME_SIZE,&savedWidth,&savedHeight));
        if(FAILED(current->GetUINT32(MF_MT_DEFAULT_STRIDE,&stride)))stride=savedWidth;
        Expect(savedWidth==outputWidth && savedHeight==outputHeight,"colored recording output dimensions");
        unsigned decodedFrames=0;double largestError=0;
        for(;;){
            DWORD flags=0;ComPtr<IMFSample> sample;Check(reader->ReadSample(stream,0,nullptr,&flags,nullptr,&sample));
            if(sample){
                ComPtr<IMFMediaBuffer> buffer;Check(sample->ConvertToContiguousBuffer(&buffer));
                BYTE* data=nullptr;DWORD bytes=0;Check(buffer->Lock(&data,nullptr,&bytes));
                const bool valid=data && savedWidth==outputWidth && savedHeight==outputHeight &&
                    stride>=savedWidth && bytes>=static_cast<uint64_t>(stride)*savedHeight*3/2;
                Expect(valid,"recorded color frame has complete NV12 planes");
                if(valid){
                    const auto patches=ReadColorPatches(data,stride,savedWidth,savedHeight);
                    for(size_t bar=0;bar<patches.size();++bar)for(size_t plane=0;plane<3;++plane)
                        largestError=(std::max)(largestError,std::abs(patches[bar][plane]-reference[bar][plane]));
                }
                Check(buffer->Unlock());++decodedFrames;
            }
            if(flags&MF_SOURCE_READERF_ENDOFSTREAM)break;
        }
        printf("%s color decoded=%u max patch Y/U/V error=%.2f levels\n",input,decodedFrames,largestError);
        Expect(decodedFrames==12,"colored recording retains all short-test frames");
        // Constant patch centers are insensitive to bilinear resize. This allows
        // H.264 quantization but catches unwanted full/limited conversion and
        // matrix conversion; metadata is checked separately above.
        Expect(largestError<=10.0,"recording preserves input luma and chroma");
    }
}
static std::vector<double> Pulses(const wchar_t* path,bool video,int* peak=nullptr) {
    ComPtr<IMFSourceReader> r; Check(MFCreateSourceReaderFromURL(path,nullptr,&r));
    Check(r->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS),FALSE));
    DWORD stream=video?MF_SOURCE_READER_FIRST_VIDEO_STREAM:MF_SOURCE_READER_FIRST_AUDIO_STREAM;
    Check(r->SetStreamSelection(stream,TRUE));
    ComPtr<IMFMediaType> type; Check(MFCreateMediaType(&type));
    Check(type->SetGUID(MF_MT_MAJOR_TYPE,video?MFMediaType_Video:MFMediaType_Audio));
    Check(type->SetGUID(MF_MT_SUBTYPE,video?MFVideoFormat_NV12:MFAudioFormat_PCM));
    if(!video){Check(type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE,16)); Check(type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS,2));Check(type->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND,48000));}
    Check(r->SetCurrentMediaType(stream,nullptr,type.Get()));
    std::vector<double> result; bool previous=false; LONGLONG last=-1;
    for(;;) {
        DWORD flags=0;LONGLONG time=0;ComPtr<IMFSample> sample;
        Check(r->ReadSample(stream,0,nullptr,&flags,&time,&sample));
        if(flags&MF_SOURCE_READERF_ENDOFSTREAM) break;
        if(!sample) continue;
        Expect(time>last,"decoded timestamps monotonic");last=time;
        ComPtr<IMFMediaBuffer> b;Check(sample->ConvertToContiguousBuffer(&b));
        BYTE* data=nullptr;DWORD len=0;Check(b->Lock(&data,nullptr,&len));
        if(video){
            const bool pulse=len && data[0]>160;
            if(pulse&&!previous) result.push_back(time/1e7);
            previous=pulse;
        }else{
            auto* pcm=reinterpret_cast<const short*>(data);
            if(peak)for(DWORD i=0;i<len/2;++i)*peak=(std::max)(*peak,std::abs(static_cast<int>(pcm[i])));
            for(DWORD i=0;i<len/4;++i){
                const bool pulse=std::abs(static_cast<int>(pcm[2*i]))>5000;
                // Debounce the sine wave and AAC pre/post ringing.
                if(pulse && (result.empty() || time/1e7+i/48000.0-result.back()>0.5))
                    result.push_back(time/1e7+i/48000.0);
            }
        }
        b->Unlock();
    }
    return result;
}
static void Inspect(const wchar_t* path,bool video) {
    ComPtr<IMFSourceReader> r;Check(MFCreateSourceReaderFromURL(path,nullptr,&r));
    Check(r->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS),FALSE));
    const DWORD stream=video?static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM):static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM);
    Check(r->SetStreamSelection(stream,TRUE));
    ComPtr<IMFMediaType> type;Check(MFCreateMediaType(&type));
    Check(type->SetGUID(MF_MT_MAJOR_TYPE,video?MFMediaType_Video:MFMediaType_Audio));
    Check(type->SetGUID(MF_MT_SUBTYPE,video?MFVideoFormat_NV12:MFAudioFormat_PCM));
    if(!video) Check(type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE,16));
    Check(r->SetCurrentMediaType(stream,nullptr,type.Get()));
    LONGLONG first=-1,end=0,last=-1,maxGap=0;unsigned count=0;uint64_t nonzero=0;
    for(;;){
        DWORD flags=0;LONGLONG time=0;ComPtr<IMFSample> sample;
        Check(r->ReadSample(stream,0,nullptr,&flags,&time,&sample));
        if(flags&MF_SOURCE_READERF_ENDOFSTREAM)break;
        if(!sample)continue;
        if(first<0)first=time;
        Expect(time>last,"file timestamps monotonic");last=time;
        if(count)maxGap=(std::max)(maxGap,time-end);
        LONGLONG duration=0;Check(sample->GetSampleDuration(&duration));end=time+duration;
        if(!video){
            ComPtr<IMFMediaBuffer> buffer;Check(sample->ConvertToContiguousBuffer(&buffer));
            BYTE* data=nullptr;DWORD bytes=0;Check(buffer->Lock(&data,nullptr,&bytes));
            auto* pcm=reinterpret_cast<short*>(data);
            for(DWORD i=0;i<bytes/2;++i)if(std::abs(static_cast<int>(pcm[i]))>100)++nonzero;
            buffer->Unlock();
        }
        ++count;
    }
    printf("%s samples=%u start=%.6fs end=%.6fs max_gap=%.3fms audible_samples=%llu\n",
        video?"video":"audio",count,first/1e7,end/1e7,maxGap/10000.0,nonzero);
    Expect(count>0,"file contains decoded stream");
}
static void VerifyContinuousAudio(const wchar_t* path,double seconds) {
    ComPtr<IMFSourceReader> reader;Check(MFCreateSourceReaderFromURL(path,nullptr,&reader));
    Check(reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS),FALSE));
    const DWORD stream=static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM);
    Check(reader->SetStreamSelection(stream,TRUE));
    ComPtr<IMFMediaType> type;Check(MFCreateMediaType(&type));
    Check(type->SetGUID(MF_MT_MAJOR_TYPE,MFMediaType_Audio));Check(type->SetGUID(MF_MT_SUBTYPE,MFAudioFormat_PCM));
    Check(type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE,16));Check(type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS,2));
    Check(type->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND,48000));Check(reader->SetCurrentMediaType(stream,nullptr,type.Get()));
    LONGLONG first=-1,end=0,maxGap=0;uint64_t frames=0,quietWindows=0;unsigned window=0;double energy=0;
    for(;;){
        DWORD flags=0;LONGLONG time=0;ComPtr<IMFSample> sample;
        Check(reader->ReadSample(stream,0,nullptr,&flags,&time,&sample));
        if(flags&MF_SOURCE_READERF_ENDOFSTREAM)break;if(!sample)continue;
        if(first<0)first=time;else maxGap=(std::max)(maxGap,std::abs(time-end));
        ComPtr<IMFMediaBuffer> buffer;Check(sample->ConvertToContiguousBuffer(&buffer));
        BYTE* data=nullptr;DWORD length=0;Check(buffer->Lock(&data,nullptr,&length));
        const auto* pcm=reinterpret_cast<const short*>(data);
        for(DWORD frame=0;frame<length/4;++frame){
            energy+=double(pcm[frame*2])*pcm[frame*2];++window;++frames;
            if(window==480){
                const double elapsed=frames/48000.0;
                if(elapsed>0.05 && elapsed<seconds-0.05 && energy/window<100.0*100.0)++quietWindows;
                window=0;energy=0;
            }
        }
        Check(buffer->Unlock());end=time+static_cast<LONGLONG>(length/4)*10000000/48000;
    }
    printf("large audio decoded=%llu frames end=%.6fs max timestamp discontinuity=%.3fms quiet windows=%llu\n",
        frames,end/1e7,maxGap/10000.0,quietWindows);
    Expect(first==0 && maxGap<=1000,"split audio keeps contiguous decoded timestamps");
    Expect(frames>=static_cast<uint64_t>(seconds*48000) && frames<=static_cast<uint64_t>(seconds*48000)+4096,
        "split audio retains sample count within AAC framing padding");
    Expect(std::abs(end/1e7-seconds)<0.1,"split audio retains expected duration");
    Expect(quietWindows==0,"split audio has no silent holes across packet boundaries");
}
static void Lifecycle(llcv::recording::Recorder& recorder,
                      const llcv::video::VideoSampleFormat& video,
                      const llcv::capture_audio::Format& audio) {
    llcv::recording::Options options;options.encoder=L"cpu";
    ComPtr<IMemAllocator> allocator;Check(CoCreateInstance(CLSID_MemoryAllocator,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&allocator)));
    ALLOCATOR_PROPERTIES requested{4,640*360*3/2,1,0},actual{};
    Check(allocator->SetProperties(&requested,&actual));Check(allocator->Commit());
    for(int cycle=0;cycle<4;++cycle){
        const auto path=std::filesystem::temp_directory_path()/
            (L"LLCV-lifecycle-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64())+L".mp4");
        recorder.Configure(video,audio,{},false,options);
        Expect(recorder.Start(path.wstring()),"restart after completed stop");
        if(cycle>0){
            for(int i=0;i<1000 && recorder.Busy()&&!recorder.Recording();++i)Sleep(10);
            Expect(recorder.Recording(),"restarted encoder ready");
        }
        std::thread vp,ap;
        std::atomic<bool> running{true};
        std::atomic<bool> producerError{false};
        if(cycle>=2){
            auto produce=[&](bool isVideo){
                try{
                    LONGLONG time=0;const LONGLONG duration=isVideo?333333:100000;
                    while(running){
                        auto sample=Sample(allocator.Get(),isVideo?640*360*3/2:1920,time,duration);
                        BYTE* data=nullptr;Check(sample->GetPointer(&data));
                        memset(data,isVideo?128:0,sample->GetActualDataLength());
                        recorder.Push(isVideo,sample.Get());time+=duration;
                        Sleep(isVideo?33:10);
                    }
                }catch(...){producerError=true;}
            };
            vp=std::thread(produce,true);ap=std::thread(produce,false);
            Sleep(150);
        }
        const auto start=std::chrono::steady_clock::now();
        recorder.Stop();
        Expect(!recorder.Recording(),"completed stop closes capture admission immediately");
        // A rejected invalid sample would fail the session if a late worker
        // publication reopened admission after Stop returned.
        if(cycle>=2)recorder.Push(true,nullptr);
        recorder.Shutdown();
        running=false;if(vp.joinable())vp.join();if(ap.joinable())ap.join();
        Expect(!producerError,"concurrent callback producers complete");
        Expect(!recorder.Busy()&&!recorder.Recording(),"stop before ready, idle, and active callbacks");
        if(cycle>=2)Expect(SUCCEEDED(recorder.Result()),"concurrent stop finalizes both streams");
        Expect(std::chrono::steady_clock::now()-start<std::chrono::seconds(5),"shutdown wakes idle worker promptly");
    }
}
static void AudioPacketBounds(llcv::recording::Recorder& recorder,
                              const llcv::video::VideoSampleFormat& video,
                              const llcv::capture_audio::Format& audio) {
    llcv::recording::RecorderTestAccess::AudioChunkAtomicity();
    llcv::recording::Options options;options.encoder=L"cpu";
    ComPtr<IMemAllocator> allocator;Check(CoCreateInstance(CLSID_MemoryAllocator,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&allocator)));
    ALLOCATOR_PROPERTIES requested{2,19200*32+4,1,0},actual{};
    Check(allocator->SetProperties(&requested,&actual));Check(allocator->Commit());
    // The oversized callback must be accepted as a whole or rejected before
    // publication, never silently truncated to one 100 ms buffer.
    for(const long length:{19201L,19200L*32+4}){
        const auto path=std::filesystem::temp_directory_path()/(L"LLCV-audio-bounds-"+
            std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(length)+L"-"+std::to_wstring(GetTickCount64())+L".mp4");
        recorder.Configure(video,audio,{},false,options);Expect(recorder.Start(path.wstring()),"audio bounds start/restart");
        for(int i=0;i<1000 && recorder.Busy()&&!recorder.Recording();++i)Sleep(10);
        Expect(recorder.Recording(),"audio bounds encoder ready");
        auto sample=Sample(allocator.Get(),length,0,static_cast<LONGLONG>(length/4)*10000000/48000);
        BYTE* data=nullptr;Check(sample->GetPointer(&data));std::memset(data,0,length);
        recorder.Push(false,sample.Get());
        Expect(!recorder.Recording(),"invalid/over-capacity audio closes admission synchronously");
        recorder.Shutdown();
        const auto status=recorder.GetSnapshot();
        Expect(FAILED(recorder.Result()) && !status.playable,"rejected audio packet publishes no playable prefix");
        Expect(status.failure==(length%4?llcv::recording::Failure::InvalidInput:llcv::recording::Failure::EncoderOverload),
            "audio alignment and bounded queue overload are distinguished");
    }
    Check(allocator->Decommit());
}
int wmain(int argc, wchar_t** argv) {
    setvbuf(stdout,nullptr,_IONBF,0);
    Check(CoInitializeEx(nullptr,COINIT_MULTITHREADED)); Check(MFStartup(MF_VERSION));
    try {
        if(argc>1&&std::wstring(argv[1])==L"--encoders"){
            const auto support=llcv::recording::ProbeEncoders();
            for(int i=0;i<3;++i){printf("codec=%d MP4=%d software=%d hardware=%zu HW(bitrate/CQP)=%d/%d CPU=%d/%d\n",i,support[i].container,support[i].software,support[i].hardware.size(),support[i].hardwareRates.bitrate,support[i].hardwareRates.quality,support[i].softwareRates.bitrate,support[i].softwareRates.quality);
                for(auto& encoder:support[i].hardware)wprintf(L"  %ls\n",encoder.name.c_str());}
            MFShutdown();CoUninitialize();return 0;
        }
        if(argc>2 && std::wstring(argv[1])==L"--inspect"){
            Inspect(argv[2],true);Inspect(argv[2],false);
            goto finished;
        }
        llcv::recording::Recorder recorder;
        llcv::video::VideoSampleFormat v{};v.valid=true;v.width=640;v.height=360;v.fps=30;v.stride=640;v.format=llcv::settings::VideoPixelFormat::Nv12;
        llcv::capture_audio::Format a{};a.channels=2;a.containerBits=a.validBits=16;a.blockAlign=4;a.path=llcv::capture_audio::Path::Direct16BitStereo;
        if(argc>1&&std::wstring(argv[1])==L"--lifecycle"){
            Lifecycle(recorder,v,a);MFShutdown();CoUninitialize();return failures?1:0;
        }
        if(argc>1&&std::wstring(argv[1])==L"--audio-bounds"){
            AudioPacketBounds(recorder,v,a);MFShutdown();CoUninitialize();return failures?1:0;
        }
        const std::wstring mode=argc>1?argv[1]:L"";
        if(mode==L"--mjpeg-color" || mode==L"--mjpeg-color-scale"){
            TestRecordingColor(mode==L"--mjpeg-color-scale",llcv::settings::VideoPixelFormat::Mjpeg);
            MFShutdown();CoUninitialize();return failures?1:0;
        }
        if(mode==L"--raw-color" || mode==L"--raw-color-scale" ||
           mode==L"--raw-yuy2-color" || mode==L"--raw-yuy2-color-scale"){
            TestRecordingColor(mode.ends_with(L"-scale"),mode.starts_with(L"--raw-yuy2")
                ?llcv::settings::VideoPixelFormat::Yuy2:llcv::settings::VideoPixelFormat::Nv12);
            MFShutdown();CoUninitialize();return failures?1:0;
        }
        const bool mjpeg=mode.starts_with(L"--mjpeg");
        const bool mjpegHd=mode==L"--mjpeg-hd";
        const bool invalidMjpeg=mode==L"--mjpeg-invalid";
        const bool arrival=mode==L"--arrival" || mode==L"--hevc-arrival" || mode==L"--mjpeg-arrival";
        const bool limited=mode==L"--limited" || mode==L"--mjpeg-limited";
        const bool split=mode==L"--split-pause" || mode==L"--gain-split" || mode==L"--mjpeg-split";
        const bool gain=argc>1 && (std::wstring(argv[1])==L"--gain"||std::wstring(argv[1])==L"--gain-split");
        const bool mute=argc>1 && std::wstring(argv[1])==L"--mute";
        const bool unaligned=argc>1 && std::wstring(argv[1])==L"--pause-unaligned";
        const bool largeAudio=mode==L"--large-audio" || mode==L"--large-audio-pause";
        const bool pause=mode==L"--pause" || mode==L"--mjpeg-pause" || mode==L"--large-audio-pause" || split || unaligned;
        const bool scale=mode==L"--scale" || mode==L"--mjpeg-scale";
        const bool hevc=mode==L"--hevc" || mode==L"--hevc-arrival" || mode==L"--mjpeg-hevc";
        const bool quality=argc>1 && (std::wstring(argv[1])==L"--quality"||std::wstring(argv[1])==L"--quality-yuy2");
        const bool padded=argc>1 && std::wstring(argv[1])==L"--padded";
        const bool yuy2=argc>1 && (std::wstring(argv[1])==L"--yuy2"||std::wstring(argv[1])==L"--quality-yuy2");
        if(padded)v.stride+=32;
        if(yuy2){v.format=llcv::settings::VideoPixelFormat::Yuy2;v.stride=v.width*2+32;}
        if (arrival) v.fps=50;
        if (argc>1 && std::wstring(argv[1])==L"--probe") {
            v.width=v.stride=2560;v.height=1440;v.fps=120;
            recorder.Configure(v,a,{});
            const auto probe=std::filesystem::temp_directory_path()/(L"LLCV-probe-"+std::to_wstring(GetTickCount64())+L".mp4");
            recorder.Start(probe.wstring());
            while(recorder.Busy()&&!recorder.Recording()) Sleep(10);
            printf("probe result=0x%08lX ready=%d\n",static_cast<unsigned long>(recorder.Result()),recorder.Recording()?1:0);
            recorder.Shutdown();return 0;
        }
        const auto path=std::filesystem::temp_directory_path()/(L"LLCV-record-test-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64())+L".mp4");
        llcv::recording::Options testOptions;testOptions.encoder=L"cpu";
        testOptions.audioVolumePercent=mute?0:gain?50:100;
        if(split){testOptions.splitMode=llcv::recording::SplitMode::Time;testOptions.splitSeconds=2;}
        if(scale){v.width=v.stride=1280;v.height=960;testOptions.maxHeight=720;}
        testOptions.rateControl=(quality||hevc)?llcv::recording::RateControl::Quality:llcv::recording::RateControl::Bitrate;
        if(limited){v.fps=60;testOptions.maxFps=30;testOptions.videoMbps=4;testOptions.audioKbps=128;}
        if(hevc){
            const auto support=llcv::recording::ProbeEncoders();
            if(!support[1].hardwareRates.quality){
                puts("SKIP: HEVC hardware quality encoder unavailable");
                MFShutdown();CoUninitialize();return 77;
            }
            testOptions.codec=llcv::recording::Codec::Hevc;testOptions.encoder.clear();
        }
        if(mjpegHd){
            const auto support=llcv::recording::ProbeEncoders();
            if(!support[0].hardwareRates.quality){
                puts("SKIP: H.264 hardware quality encoder unavailable");
                MFShutdown();CoUninitialize();return 77;
            }
            v.width=1920;v.height=1080;v.fps=60;
            testOptions.encoder.clear();testOptions.rateControl=llcv::recording::RateControl::Quality;
        }
        std::vector<BYTE> darkJpeg,brightJpeg;
        if(mjpeg){
            darkJpeg=JpegFrame(v.width,v.height,false,mjpegHd);brightJpeg=JpegFrame(v.width,v.height,true,mjpegHd);
            v.format=llcv::settings::VideoPixelFormat::Mjpeg;v.stride=0;
            v.bytes=static_cast<DWORD>((std::max)(darkJpeg.size(),brightJpeg.size()));
        }
        const auto inputColor=llcv::video_color::Resolve(mjpeg,v.width,v.height,{},{});
        recorder.Configure(v,a,inputColor,arrival,testOptions);
        Expect(recorder.Start(path.wstring()),"start");
        for(int i=0;i<1000&&!recorder.Recording()&&recorder.Busy();++i) Sleep(10);
        Expect(recorder.Recording(),"encoder ready");
        ComPtr<IMemAllocator> allocator;Check(CoCreateInstance(CLSID_MemoryAllocator,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&allocator)));
        const long frameBytes=mjpeg?static_cast<long>(v.bytes):static_cast<long>(v.stride*(yuy2?v.height:v.height*3/2));
        ALLOCATOR_PROPERTIES requested{2,frameBytes,1,0},actual{};Check(allocator->SetProperties(&requested,&actual));Check(allocator->Commit());
        if(invalidMjpeg){
            auto bad=Sample(allocator.Get(),frameBytes,0,10000000/v.fps);BYTE* data=nullptr;Check(bad->GetPointer(&data));
            std::memset(data,0,frameBytes);recorder.Push(true,bad.Get());
            for(int i=0;i<1000 && recorder.Recording();++i)Sleep(1);
            recorder.Shutdown();
            Expect(FAILED(recorder.Result()) && recorder.GetSnapshot().failure==llcv::recording::Failure::InvalidInput,
                "malformed MJPEG stops only recording with an input failure");
            Check(allocator->Decommit());goto finished;
        }
        auto start=std::chrono::steady_clock::now();double maxPushUs=0,totalPushUs=0,maxAudioPushUs=0,totalAudioPushUs=0;unsigned pushes=0;
        double maxScheduleLatenessMs=0,maxVideoGapMs=0,minVideoGapMs=-1;
        unsigned catchUpTicks=0,catchUpRun=0,maxCatchUpRun=0;
        auto previousVideoPush=start;bool havePreviousVideoPush=false;
        std::vector<double> flashWall;
        double audioWall=0;
        int audioFrame=0;unsigned audioPacket=0;
        constexpr int audioPacketFrames[]={4800,4801,12000,5904,9600,7201};
        auto wall=[] { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); };
        for(int tick=0;tick<600 && recorder.Recording();++tick){
            const auto deadline=start+std::chrono::milliseconds(tick*10);
            std::this_thread::sleep_until(deadline);
            // Observe scheduler stalls without changing the original fixture
            // clock or its deliberately bursty timestamped video delivery.
            const double latenessMs=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-deadline).count();
            maxScheduleLatenessMs=(std::max)(maxScheduleLatenessMs,latenessMs);
            if(latenessMs>=10){++catchUpTicks;++catchUpRun;maxCatchUpRun=(std::max)(maxCatchUpRun,catchUpRun);}
            else catchUpRun=0;
            if(pause&&(tick==(unaligned?185:180)||tick==(unaligned?255:250))){
                const bool pausing=tick<200;
                Expect(recorder.SetPaused(pausing),"pause/resume accepted");
                Expect(recorder.GetSnapshot().paused==pausing,"pause snapshot changes immediately");
            }
            if(tick%(arrival?2:10)==0){
                // 30fps timestamps / 3 frames every 100ms, deliberately bursty.
                for(int j=0;j<(arrival?1:v.fps/10);++j){
                    int frame=arrival?tick/2:tick/10*(v.fps/10)+j; LONGLONG time=frame*10000000LL/v.fps;
                    auto s=Sample(allocator.Get(),frameBytes,time,10000000/v.fps);BYTE* d=nullptr;s->GetPointer(&d);
                    if(arrival) s->SetTime(nullptr,nullptr);
                    const bool pulse=(time>=10000000&&time<11000000)||(time>=30000000&&time<31000000)||(time>=50000000&&time<51000000);
                    if(mjpeg){
                        const auto& jpeg=pulse?brightJpeg:darkJpeg;
                        std::memcpy(d,jpeg.data(),jpeg.size());Check(s->SetActualDataLength(static_cast<long>(jpeg.size())));
                    }else{
                    memset(d,0xff,frameBytes); // Padding must not enter encoded rows.
                    for(int row=0;row<(yuy2?v.height:v.height*3/2);++row){
                        auto* line=d+row*v.stride;
                        if(yuy2)for(int x=0;x<v.width;++x){line[x*2]=pulse?220:16;line[x*2+1]=128;}
                        else memset(line,row<v.height?(pulse?220:16):128,v.width);
                    }
                    }
                    if(arrival && (time==10000000 || time==30000000 || time==50000000)) flashWall.push_back(wall());
                    const auto t=std::chrono::steady_clock::now();
                    recorder.Push(true,s.Get());
                    const double us=std::chrono::duration<double,std::micro>(std::chrono::steady_clock::now()-t).count();
                    if(havePreviousVideoPush){
                        const double gapMs=std::chrono::duration<double,std::milli>(t-previousVideoPush).count();
                        maxVideoGapMs=(std::max)(maxVideoGapMs,gapMs);
                        minVideoGapMs=minVideoGapMs<0?gapMs:(std::min)(minVideoGapMs,gapMs);
                    }
                    previousVideoPush=t;havePreviousVideoPush=true;
                    if(mjpeg){
                        const auto& jpeg=pulse?brightJpeg:darkJpeg;
                        Expect(std::memcmp(d,jpeg.data(),jpeg.size())==0,"recording leaves compressed playback samples untouched");
                    }
                    maxPushUs=(std::max)(maxPushUs,us);totalPushUs+=us;++pushes;
                }
            }
            while(audioFrame<600*480 && (audioFrame<=tick*480 || tick==599)){
                int frames=largeAudio?audioPacketFrames[audioPacket%std::size(audioPacketFrames)]:480;
                frames=(std::min)(frames,600*480-audioFrame);
                if(largeAudio && pause)for(const int cut:{180*480,250*480})
                    if(audioFrame<cut)frames=(std::min)(frames,cut-audioFrame);
                auto s=Sample(allocator.Get(),frames*4,static_cast<LONGLONG>(audioFrame)*10000000/48000,
                    static_cast<LONGLONG>(frames)*10000000/48000);
                BYTE* d=nullptr;Check(s->GetPointer(&d));auto* pcm=reinterpret_cast<short*>(d);
                if(arrival)s->SetTime(nullptr,nullptr);
                auto expectedAudio=[&](int i){
                    const int frame=audioFrame+i;
                    const bool pulse=(frame>=48000&&frame<52800)||(frame>=144000&&frame<148800)||(frame>=240000&&frame<244800);
                    return static_cast<short>((pulse?12000:largeAudio?1000:0)*sin(i*6.28318530718*1000/48000));
                };
                for(int i=0;i<frames;++i)pcm[i*2]=pcm[i*2+1]=expectedAudio(i);
                if(tick==0)audioWall=wall();
                const auto audioPushStart=std::chrono::steady_clock::now();
                recorder.Push(false,s.Get());
                const double audioPushUs=std::chrono::duration<double,std::micro>(std::chrono::steady_clock::now()-audioPushStart).count();
                totalAudioPushUs+=audioPushUs;maxAudioPushUs=(std::max)(maxAudioPushUs,audioPushUs);
                if(gain||mute||largeAudio)for(int i=0;i<frames;++i){
                    const short expected=expectedAudio(i);
                    Expect(pcm[i*2]==expected&&pcm[i*2+1]==expected,"recording leaves source playback samples untouched");
                }
                audioFrame+=frames;++audioPacket;
            }
        }
        recorder.Stop();recorder.Shutdown();
        printf("record result=0x%08lX, video enqueue mean=%.2fus max=%.2fus\n",static_cast<unsigned long>(recorder.Result()),totalPushUs/(std::max)(1u,pushes),maxPushUs);
        printf("audio enqueue packets=%u mean=%.2fus max=%.2fus\n",audioPacket,totalAudioPushUs/(std::max)(1u,audioPacket),maxAudioPushUs);
        printf("input scheduler max lateness=%.3fms catch-up ticks=%u longest run=%u video gap min/max=%.3f/%.3fms\n",
            maxScheduleLatenessMs,catchUpTicks,maxCatchUpRun,minVideoGapMs,maxVideoGapMs);
        Expect(SUCCEEDED(recorder.Result()),"record/finalize");
        // Derive the expected count from the complete six-second fixture, not
        // the number accepted by the recorder (which would hide an early stop).
        // The pause fixtures omit seven 100 ms bursts: 21 of 180 input frames.
        const unsigned expectedVideoFrames=pause?159u:6u*static_cast<unsigned>(llcv::recording::OutputFps(testOptions,v.fps));
        Expect(pushes==6u*static_cast<unsigned>(v.fps),"fixture delivers every scheduled input video frame");
        if(split){
            unsigned frames=0,parts=0,events=0;
            for(unsigned part=1;;++part){
                const auto segment=llcv::recording::RecordingPartPath(path.wstring(),part);
                if(!std::filesystem::exists(segment))break;
                ++parts;
                ComPtr<IMFSourceReader> reader;Check(MFCreateSourceReaderFromURL(segment.c_str(),nullptr,&reader));
                for(;;){ComPtr<IMFSample> sample;DWORD flags=0;Check(reader->ReadSample(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM),0,nullptr,&flags,nullptr,&sample));
                    if(sample)++frames;if(flags&MF_SOURCE_READERF_ENDOFSTREAM)break;}
                const auto vp=Pulses(segment.c_str(),true),ap=Pulses(segment.c_str(),false);
                Expect(vp.size()==ap.size(),"split decoded flash/tone counts agree");
                events+=static_cast<unsigned>(vp.size());
                for(size_t i=0;i<std::min(vp.size(),ap.size());++i)Expect(std::abs(vp[i]-ap[i])<0.04,"split decoded A/V skew below 40 ms");
                printf("split: %ls\n",segment.c_str());
            }
            printf("stored video frames=%u expected=%u across %u parts\n",frames,expectedVideoFrames,parts);
            Expect(parts>=2&&frames==expectedVideoFrames&&events==3,"split and pause preserve every intended frame/event");
            goto finished;
        }
        if(SUCCEEDED(recorder.Result())){
            if(largeAudio)VerifyContinuousAudio(path.c_str(),pause?5.3:6.0);
            {
                ComPtr<IMFSourceReader> reader;Check(MFCreateSourceReaderFromURL(path.c_str(),nullptr,&reader));
                ComPtr<IMFMediaType> media;Check(reader->GetNativeMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM),0,&media));
                UINT32 width=0,height=0;Check(MFGetAttributeSize(media.Get(),MF_MT_FRAME_SIZE,&width,&height));
                {
                    printf("saved native dimensions=%ux%u\n",width,height);
                    for(const auto key:{MF_MT_MINIMUM_DISPLAY_APERTURE,MF_MT_GEOMETRIC_APERTURE}){
                        MFVideoArea aperture{};UINT32 bytes=0;
                        if(SUCCEEDED(media->GetBlob(key,reinterpret_cast<UINT8*>(&aperture),sizeof(aperture),&bytes)) && bytes==sizeof(aperture)){
                            printf("display aperture=%ldx%ld offset=%d,%d\n",aperture.Area.cx,aperture.Area.cy,
                                aperture.OffsetX.value,aperture.OffsetY.value);
                            const bool valid=aperture.OffsetX.value>=0 && aperture.OffsetY.value>=0 &&
                                aperture.Area.cx>0 && aperture.Area.cy>0 &&
                                static_cast<UINT32>(aperture.OffsetX.value+aperture.Area.cx)<=width &&
                                static_cast<UINT32>(aperture.OffsetY.value+aperture.Area.cy)<=height;
                            Expect(valid,"saved display aperture fits the encoded frame");
                            if(valid){width=aperture.Area.cx;height=aperture.Area.cy;}
                            break;
                        }
                    }
                }
                Expect(width==(scale?960u:mjpegHd?1920u:640u)&&height==(scale?720u:mjpegHd?1080u:360u),"aspect-preserving recording size");
                unsigned frames=0;
                for(;;){ComPtr<IMFSample> sample;DWORD flags=0;Check(reader->ReadSample(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM),0,nullptr,&flags,nullptr,&sample));
                    if(sample)++frames;if(flags&MF_SOURCE_READERF_ENDOFSTREAM)break;}
                printf("stored video frames=%u expected=%u input=%u fps=%d output fps=%d\n",
                    frames,expectedVideoFrames,pushes,v.fps,llcv::recording::OutputFps(testOptions,v.fps));
                Expect(frames==expectedVideoFrames,"every intended video frame is stored outside intentional pause/frame-rate cap");
            }
            if(limited){
                ComPtr<IMFSourceReader> reader;Check(MFCreateSourceReaderFromURL(path.c_str(),nullptr,&reader));
                ComPtr<IMFMediaType> media;Check(reader->GetNativeMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM),0,&media));
                UINT32 num=0,den=0;Check(MFGetAttributeRatio(media.Get(),MF_MT_FRAME_RATE,&num,&den));
                Expect(num==30*den,"output frame rate is capped at 30, not capture rate");
            }
            int peak=0;
            auto vp=Pulses(path.c_str(),true),ap=Pulses(path.c_str(),false,&peak);
            printf("decoded audio peak=%d volume=%d%%\n",peak,testOptions.audioVolumePercent);
            if(mute)Expect(vp.size()==3&&ap.empty()&&peak==0,"muted AAC keeps video events and decodes to silence");
            else Expect(vp.size()==3&&ap.size()==3,"three decoded synchronized flash/tone events");
            if(gain)Expect(peak>=5400&&peak<=7200,"half-volume AAC retains expected amplitude");
            for(size_t i=0;i<(std::min)(vp.size(),ap.size());++i){
                if(pause)Expect(std::abs(vp[i]-(1.0+i*2-(i?0.7:0)))<0.04,"paused time removed from saved timeline");
                printf("event %zu video=%.6fs audio=%.6fs skew=%.3fms\n",i,vp[i],ap[i],(ap[i]-vp[i])*1000);
                Expect(std::abs(ap[i]-vp[i])<0.040,"decoded A/V skew below 40ms");
                if(arrival && flashWall.size()==3) {
                    const double expected=audioWall-0.010+(1.0+i*2)-(flashWall[i]-1.0/v.fps);
                    printf("arrival scheduler skew=%.3fms, encoding residual=%.3fms\n",expected*1000,(ap[i]-vp[i]-expected)*1000);
                    Expect(std::abs(ap[i]-vp[i]-expected)<0.005,"arrival clock preserves measured source timing within 5ms");
                }
            }
            if(!arrival&&vp.size()==3&&ap.size()==3) Expect(std::abs((ap[2]-vp[2])-(ap[0]-vp[0]))<0.005,"no growing sync drift");
        }
        recorder.Configure(v,a,{});Expect(recorder.Start(path.wstring()),"existing file async start");
        while(recorder.Busy()) Sleep(10);
        Expect(FAILED(recorder.Result()),"existing file must not be overwritten");
        v.format=llcv::settings::VideoPixelFormat::P010;recorder.Configure(v,a,{});
        Expect(!recorder.Start(path.wstring()+L".hdr.mp4"),"HDR rejected");
        v.format=llcv::settings::VideoPixelFormat::Nv12;a.channels=6;recorder.Configure(v,a,{});
        Expect(!recorder.Start(path.wstring()+L".51.mp4"),"unsupported audio rejected");
        printf("test recording: %ls\n",path.c_str());
    } catch(HRESULT hr){printf("HRESULT 0x%08lX\n",static_cast<unsigned long>(hr));++failures;}
finished:
    MFShutdown();CoUninitialize();return failures?1:0;
}
