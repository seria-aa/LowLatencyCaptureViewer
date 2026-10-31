#include "recording/RecordingOptions.h"
#include "recording/RecordingAudioGain.h"
#include <array>
#include <windows.h>
#include <filesystem>
#include <cstdio>
#include <cstdlib>
using namespace llcv::recording;
static void Check(bool b,const char* s){if(!b){printf("FAIL %s\n",s);std::abort();}}
int main(){
    Options o;Check(Valid(o),"valid defaults");
    Check(o.audioVolumePercent==100,"recording gain defaults to unchanged");
    std::array<int16_t,8> pcm{-32768,-20000,-2,0,2,10000,20000,32767};
    const auto original=pcm;
    ApplyRecordingAudioGain(pcm.data(),sizeof(pcm),100);
    Check(pcm==original,"unity is bit exact");
    ApplyRecordingAudioGain(pcm.data(),sizeof(pcm),50);
    for(size_t i=0;i<pcm.size();++i)Check(pcm[i]==original[i]/2,"both PCM channels attenuated without overflow");
    ApplyRecordingAudioGain(pcm.data(),sizeof(pcm),0);
    for(auto sample:pcm)Check(sample==0,"mute preserves sample count as silence");
    auto invalidVolume=o;invalidVolume.audioVolumePercent=-1;Check(!Valid(invalidVolume),"negative recording volume rejected");
    invalidVolume.audioVolumePercent=101;Check(!Valid(invalidVolume),"recording boost rejected");
    Check(o.maxFps==60&&o.maxHeight==1080&&OutputFps(o,120)==60,"new defaults cap recording at 1080p60");
    const auto capped=OutputSize(o,3840,2160),small=OutputSize(o,1280,720);
    Check(capped.width==1920&&capped.height==1080&&small.width==1280&&small.height==720&&OutputFps(o,30)==30,
        "default caps downscale 4K but do not upscale or duplicate low-rate input");
    Check(LoadOptions(L"")==o,"empty profile path uses new defaults");
    o.maxFps=0;Check(OutputFps(o,120)==120,"explicit native rate unchanged");
    Check(o.rateControl==RateControl::Quality&&o.quality==llcv::recording::Quality::Balanced,"new profiles default to balanced CQP");
    Check(QualityQp(llcv::recording::Quality::Compact)==26&&QualityQp(o.quality)==23&&QualityQp(llcv::recording::Quality::High)==20,"quality presets have ordered QPs");
    o.maxFps=60;Check(OutputFps(o,40)==40&&OutputFps(o,120)==60,"cap never upsamples");
    o.maxFps=30;Check(OutputFps(o,40)==30,"40 to 30 cap");
    for(int fps:{30,60})for(int source:{30,40,60,120,144,240}){
        FrameLimiter limiter{fps<source?fps:0};int count=0;
        for(int frame=0;frame<source*6;++frame)count+=limiter.Accept(frame*10000000LL/source)?1:0;
        Check(count==std::min(fps,source)*6,"fractional tick rounding preserves intended frame count");
    }
    FrameLimiter limiter{30};Check(limiter.Accept(1000)&&!limiter.Accept(1000)&&!limiter.Accept(999),"duplicate/backward timestamp rejected");
    FrameLimiter jittered{60,120};
    for(int frame=0;frame<240;++frame){
        const int64_t jitter=frame?((frame/2)%2?10000:-10000):0;
        Check(jittered.Accept(frame*10000000LL/120+jitter)==(frame%2==0),"small arrival jitter preserves 120 to 60 cadence");
    }
    for(int rate:{2,20,100})for(int audio:{128,192})for(int fps:{0,30,60})for(auto codec:{Codec::H264,Codec::Hevc,Codec::Av1}){
        Options reserve;reserve.videoMbps=rate;reserve.audioKbps=audio;reserve.maxFps=fps;reserve.codec=codec;
        Check(ReserveBytes(reserve)==512ull*1024*1024,"CQP finalization reserve stays fixed");
        reserve.rateControl=RateControl::Bitrate;
        Check(ReserveBytes(reserve)==std::max(256ull*1024*1024,(uint64_t(rate)*1000000+uint64_t(audio)*1000)*30/8),
              "bitrate finalization reserve includes AAC and does not depend on codec or frame cap");
    }
    Check(ReserveBytes(o)>=256ull*1024*1024,"low space reserve");
    auto path=std::filesystem::temp_directory_path()/(L"llcv-record-options-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64())+L".ini");
    Check(LoadOptions(path.wstring())==Options{},"missing profile uses 1080p60 defaults");
    Options native;native.maxFps=0;native.maxHeight=0;
    Check(SaveOptions(native,path.wstring())&&LoadOptions(path.wstring())==native,"saved native size and rate survive default change");
    o.codec=Codec::Hevc;o.videoMbps=12;o.audioKbps=128;o.encoder=L"cpu";o.folder=L"D:\\녹화 日本語 \U0001F3AE";
    o.audioVolumePercent=40;
    o.maxHeight=720;o.splitMode=SplitMode::Time;o.splitSeconds=900;o.splitMiB=2048;
    Check(SaveOptions(o,path.wstring()),"save unicode profile");Check(LoadOptions(path.wstring())==o,"unicode profile round trip");
    o.codec=Codec::Av1;o.encoder.clear();o.maxFps=60;Check(SaveOptions(o,path.wstring()),"replace profile");Check(LoadOptions(path.wstring())==o,"replace round trip");
    auto bad=o;bad.maxFps=90;Check(!SaveOptions(bad,path.wstring())&&LoadOptions(path.wstring())==o,"invalid value cannot overwrite accepted settings");
    bad=o;bad.folder+=L"\nCodec=0";Check(!Valid(bad),"profile newline injection rejected");
    bad=o;bad.encoder=L"not-an-encoder";Check(!Valid(bad),"invalid encoder policy rejected");
    bad=o;bad.rateControl=static_cast<RateControl>(2);Check(!Valid(bad),"invalid rate mode rejected");
    bad=o;bad.quality=static_cast<llcv::recording::Quality>(3);Check(!Valid(bad),"invalid quality rejected");
    bad=o;bad.maxHeight=900;Check(!Valid(bad),"invalid resolution rejected");
    bad=o;bad.splitMode=static_cast<SplitMode>(3);Check(!Valid(bad),"invalid split mode rejected");
    bad=o;bad.splitSeconds=0;Check(!Valid(bad),"zero split time rejected");
    bad=o;bad.splitMiB=0;Check(!Valid(bad),"zero split size rejected");
    for(auto mode:{SplitMode::None,SplitMode::Time,SplitMode::Size}){o.splitMode=mode;Check(SaveOptions(o,path.wstring())&&LoadOptions(path.wstring())==o,"split policy round trip");}
    o.rateControl=RateControl::Bitrate;o.quality=llcv::recording::Quality::High;
    Check(SaveOptions(o,path.wstring())&&LoadOptions(path.wstring())==o,"both rate modes round trip");
    const auto corrupt = [&](const wchar_t* key, const wchar_t* value, auto member, auto fallback) {
        Check(SaveOptions(o,path.wstring()),"reset isolated corruption fixture");
        Check(WritePrivateProfileStringW(L"Recording",key,value,path.c_str())!=FALSE,"inject one invalid field");
        auto expected=o; expected.*member=fallback;
        Check(LoadOptions(path.wstring())==expected,"only invalid field recovers; all other preferences survive");
    };
    corrupt(L"Codec",L"3",&Options::codec,Codec::H264);
    corrupt(L"MaxFps",L"90",&Options::maxFps,60);
    corrupt(L"MaxHeight",L"900",&Options::maxHeight,1080);
    corrupt(L"SplitMode",L"-1",&Options::splitMode,SplitMode::None);
    corrupt(L"SplitSeconds",L"0",&Options::splitSeconds,1800);
    corrupt(L"SplitMiB",L"1048577",&Options::splitMiB,4096);
    corrupt(L"VideoMbps",L"101",&Options::videoMbps,20);
    corrupt(L"AudioKbps",L"256",&Options::audioKbps,192);
    corrupt(L"AudioVolumePercent",L"101",&Options::audioVolumePercent,100);
    corrupt(L"Quality",L"7",&Options::quality,llcv::recording::Quality::Balanced);
    corrupt(L"RateControl",L"2",&Options::rateControl,RateControl::Bitrate);
    corrupt(L"Encoder",L"not-an-encoder",&Options::encoder,std::wstring{});
    for(const auto* value:{L"abc",L"30junk",L"0x1e",L"-1",L"9999999999999999999999999999999999999999999999999999999999999999999999"})
        corrupt(L"MaxFps",value,&Options::maxFps,60);
    Check(SaveOptions(o,path.wstring()),"restore legacy migration fixture");
    Check(WritePrivateProfileStringW(L"Recording",L"RateControl",nullptr,path.c_str())!=FALSE,"make legacy profile");
    Check(LoadOptions(path.wstring()).rateControl==RateControl::Bitrate,"legacy bitrate profile is not silently switched to CQP");
    Check(WritePrivateProfileStringW(L"Recording",L"AudioVolumePercent",nullptr,path.c_str())!=FALSE,"make legacy recording gain profile");
    Check(LoadOptions(path.wstring()).audioVolumePercent==100,"legacy recording keeps original audio level");
    for(const auto* key:{L"MaxFps",L"MaxHeight",L"SplitMode",L"SplitSeconds",L"SplitMiB"})
        Check(WritePrivateProfileStringW(L"Recording",key,nullptr,path.c_str())!=FALSE,"remove new fields from legacy profile");
    const auto migrated=LoadOptions(path.wstring());
    Check(migrated.maxFps==0&&migrated.maxHeight==0&&migrated.splitMode==SplitMode::None&&migrated.folder==o.folder&&migrated.codec==o.codec,
        "legacy settings keep native resolution and no splitting without losing preferences");
    std::filesystem::remove(path); // Exact unique file created by this test.
    puts("Recording options: rate caps, size reserve, Unicode persistence and invalid settings passed.");
}
