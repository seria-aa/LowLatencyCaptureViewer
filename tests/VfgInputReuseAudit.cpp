// Opt-in SDK comparison using synthetic images; no capture device or profiles.
#include <cuda_runtime.h>
#include <windows.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <numeric>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>
#include "video/RifeHalfTest.h"
#include "video/VfgInputCache.h"
#ifdef LLCV_VFG_AMPERE_COMPAT
#include "../experiments/frame-generation/VfgAmpereCompat.h"
#endif

namespace llcv::rife_test {
namespace {
void Check(bool value,const char* stage) {if(!value)throw std::runtime_error(stage);}
void Cu(cudaError_t result,const char* stage) {
    if(result!=cudaSuccess)throw std::runtime_error(std::string(stage)+": "+cudaGetErrorString(result));
}
template<class T>T Bind(HMODULE module,const char* name) {
    auto proc=reinterpret_cast<T>(GetProcAddress(module,name));
    Check(proc!=nullptr,name);
    return proc;
}
#include "video/VfgLive.inl"

struct Array {
    cudaArray_t value=nullptr;
    void Init(unsigned w,unsigned h) {
        const auto format=cudaCreateChannelDesc<uchar4>();
        Cu(cudaMallocArray(&value,&format,w,h),"allocate RGBA array");
    }
    ~Array(){if(value)cudaFreeArray(value);}
};
struct Timing {
    cudaStream_t stream=nullptr;
    cudaEvent_t begin=nullptr,end=nullptr;
    HANDLE frameTimer=nullptr;
    void Init() {
        Cu(cudaStreamCreateWithFlags(&stream,cudaStreamNonBlocking),"create audit stream");
        Cu(cudaEventCreate(&begin),"create begin event");
        Cu(cudaEventCreateWithFlags(&end,cudaEventBlockingSync),"create end event");
        frameTimer=CreateWaitableTimerExW(nullptr,nullptr,CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                          TIMER_MODIFY_STATE|SYNCHRONIZE);
    }
    void WaitForFrame(std::chrono::steady_clock::time_point due) const {
        const auto remaining=std::chrono::duration_cast<std::chrono::nanoseconds>(
            due-std::chrono::steady_clock::now()).count()/100;
        if(remaining<=0)return;
        LARGE_INTEGER delay{};
        delay.QuadPart=-remaining;
        if(frameTimer&&SetWaitableTimer(frameTimer,&delay,0,nullptr,nullptr,FALSE)){
            Check(WaitForSingleObject(frameTimer,INFINITE)==WAIT_OBJECT_0,"wait for audit frame");
        }else std::this_thread::sleep_until(due);
    }
    ~Timing(){
        if(frameTimer)CloseHandle(frameTimer);
        if(begin)cudaEventDestroy(begin);
        if(end)cudaEventDestroy(end);
        if(stream)cudaStreamDestroy(stream);
    }
};

// Baseline: the shipped path copied both inputs to fixed previous/current
// buffers every time. Both variants use the same vendor call and output copy.
void ProcessLegacy(VfgLive& vfg,const std::array<cudaArray_t,2>& input,
                   unsigned current,cudaArray_t out,cudaStream_t stream) {
    const size_t row=size_t(vfg.w)*4;
    for(unsigned i=0;i<2;++i)
        Cu(cudaMemcpy2DFromArrayAsync(vfg.rgba[i],row,input[i?current:1-current],
            0,0,row,vfg.h,cudaMemcpyDeviceToDevice,stream),"legacy input copy");
    VfgLive::Verify(vfg.bind(vfg.effect,"SrcImage0",vfg.image[0]),"legacy previous");
    VfgLive::Verify(vfg.bind(vfg.effect,"SrcImage1",vfg.image[1]),"legacy current");
    VfgLive::Verify(vfg.set(vfg.effect,"ShotChange",0),"legacy shot");
    VfgLive::Verify(vfg.set(vfg.effect,"FrameIndex",1),"legacy midpoint");
    VfgLive::Verify(vfg.run(vfg.effect,1),"legacy run");
    Cu(cudaMemcpy2DToArrayAsync(out,0,0,vfg.rgba[2],row,row,vfg.h,cudaMemcpyDeviceToDevice,stream),"legacy output copy");
}

enum class InputPath { Fixed, Rotating, Reuse };
constexpr const char* PathName(InputPath path) {
    switch(path){
        case InputPath::Fixed: return "fixed (2 copies)";
        case InputPath::Rotating: return "rotating (2 copies)";
        case InputPath::Reuse: return "reuse (1 copy)";
    }
    return "unknown";
}
struct Sample {
    float gpuMs=0;
    float readyMs=0;
};
void PrintStats(const std::vector<Sample>& samples,float Sample::* metric,float budget) {
    std::vector<float> values;
    values.reserve(samples.size());
    for(const auto& sample:samples)values.push_back(sample.*metric);
    std::sort(values.begin(),values.end());
    const size_t n=values.size();
    std::printf("n=%zu avg=%.3f p50=%.3f p95=%.3f p99=%.3f >%.2fms=%zu",
        n,std::accumulate(values.begin(),values.end(),0.0)/double(n),
        values[n/2],values[(n-1)*95/100],values[(n-1)*99/100],double(budget),
        static_cast<size_t>(std::count_if(values.begin(),values.end(),[=](float ms){return ms>budget;})));
}

int RunAudit(const std::filesystem::path& dir,unsigned w,unsigned h,bool medium,unsigned fps,bool retest) {
    using Clock=std::chrono::steady_clock;
    constexpr int adapter=0;
    Cu(cudaSetDevice(adapter),"select audit GPU");
    cudaDeviceProp prop{};
    Cu(cudaGetDeviceProperties(&prop,adapter),"audit GPU");
    std::printf("%s; %ux%u %s; synthetic RGBA input (%u fps, zero means continuous)\n",
        prop.name,w,h,medium?"medium":"low",fps);
    const unsigned blockFrames=fps?fps:60;
    const auto framePeriod=std::chrono::nanoseconds(1000000000/blockFrames);
    const float budget=500.0f/static_cast<float>(blockFrames);
    Timing timing;
    timing.Init();
    const size_t row=size_t(w)*sizeof(uchar4);
    std::array<Array,4> patterns;
    std::array<Array,2> slots;
    Array output;
    for(auto& slot:slots)slot.Init(w,h);
    output.Init(w,h);
    {
        std::vector<uchar4> pixels(size_t(w)*h);
        for(unsigned p=0;p<patterns.size();++p){
            patterns[p].Init(w,h);
            for(unsigned y=0;y<h;++y)for(unsigned x=0;x<w;++x){
                const bool box=x>w/3+13*p&&x<w/2+13*p&&y>h/4&&y<h/2;
                const bool grid=((x+8*p)/32+y/24+p/3)%2==0;
                pixels[size_t(y)*w+x]=make_uchar4(
                    box?240:grid?40:180,box?25:grid?140:65,p==3?230:grid?60:110,255);
            }
            Cu(cudaMemcpy2DToArray(patterns[p].value,0,0,pixels.data(),row,row,h,cudaMemcpyHostToDevice),"upload synthetic input");
        }
    }
    const std::array<cudaArray_t,2> inputs{slots[0].value,slots[1].value};
    std::vector<std::vector<unsigned char>> reference;
    size_t compared=0;
    auto block=[&](InputPath path,bool compare,VfgLive& vfg,bool collect=false,bool configured=false){
        std::vector<Sample> times;
        times.reserve(blockFrames);
        // The old path writes both buffers in fixed role order.
        vfg.inputCache={};
        if(!configured)vfg.reuseInputs=path!=InputPath::Fixed;
        // Retire work before the effect or its buffers can unwind on an error.
        struct Drain {cudaStream_t stream; ~Drain(){(void)cudaStreamSynchronize(stream);}} drain{timing.stream};
        std::array<uint64_t,2> generations{};
        unsigned current=0;
        unsigned measured=0;
        auto due=Clock::now();
        std::vector<unsigned char> pixels(compare?row*h:0);
        const unsigned frames=compare?14:13+blockFrames;
        for(unsigned f=0;f<frames;++f){
            if(!compare&&fps){
                timing.WaitForFrame(due);
                due+=framePeriod;
            }
            const auto arrival=Clock::now();
            const auto pattern=patterns[(f+f/5)%patterns.size()].value;
            cudaMemcpy3DParms overwrite{};
            overwrite.srcArray=pattern;
            overwrite.dstArray=inputs[current];
            overwrite.extent=make_cudaExtent(w,h,1);
            overwrite.kind=cudaMemcpyDeviceToDevice;
            Cu(cudaMemcpy3DAsync(&overwrite,timing.stream),"overwrite shared input slot");
            generations[current]=f+1;
            // A seed updates the source slot but does not
            // enqueue VFG. The next pair must not reuse the old slot contents.
            if(f!=0&&(!compare||f!=7)){
                Cu(cudaEventRecord(timing.begin,timing.stream),"record begin");
                if(path==InputPath::Rotating)vfg.inputCache={};
                if(path!=InputPath::Fixed||configured)vfg.Process(inputs,generations,current,output.value,timing.stream);
                else ProcessLegacy(vfg,inputs,current,output.value,timing.stream);
                Cu(cudaEventRecord(timing.end,timing.stream),"record end");
                Cu(cudaEventSynchronize(timing.end),"retire audit frame");
                const auto ready=Clock::now();
                if(compare){
                    Cu(cudaMemcpy2DFromArray(pixels.data(),row,output.value,0,0,row,h,cudaMemcpyDeviceToHost),"read generated image");
                    if(collect)reference.push_back(pixels);
                    else {
                        Check(measured<reference.size(),"unexpected generated frame");
                        if(pixels!=reference[measured]){
                            const auto mismatch=std::mismatch(pixels.begin(),pixels.end(),reference[measured].begin());
                            const auto first=static_cast<size_t>(mismatch.first-pixels.begin());
                            std::fprintf(stderr,"%s frame %u first differs at byte %zu: %u vs %u\n",
                                configured?"configured":PathName(path),f,first,
                                unsigned(*mismatch.first),unsigned(*mismatch.second));
                            throw std::runtime_error("generated RGBA bytes differ");
                        }
                        if(path==InputPath::Reuse)compared+=pixels.size();
                    }
                }else if(f>12){
                    float ms=0;
                    Cu(cudaEventElapsedTime(&ms,timing.begin,timing.end),"read GPU time");
                    times.push_back({ms,std::chrono::duration<float,std::milli>(ready-arrival).count()});
                }
                ++measured;
            }
            current=1-current;
        }
        return times;
    };
    for(unsigned check=0;check<(retest?5u:4u);++check){
        VfgLive vfg;
        vfg.Init(dir,adapter,w,h,medium,timing.stream);
        block(check==2?InputPath::Reuse:check==4?InputPath::Rotating:InputPath::Fixed,true,vfg,check==0,check==3);
    }
    Check(compared==12*row*h,"missing comparison samples");
    std::printf("PASS: 12 generated frames, %zu RGBA bytes identical (including reseed); configured path matched too\n",compared);
    std::array<std::vector<Sample>,3> pooled;
    {
        // Share a warmed vendor instance so model loading isn't a variable
        // between the paired one-second timing blocks.
        VfgLive vfg;
        vfg.Init(dir,adapter,w,h,medium,timing.stream);
        auto sample=[&](InputPath path){
            const auto times=block(path,false,vfg);
            if(retest){
                std::printf("block %s ready: ",PathName(path));
                PrintStats(times,&Sample::readyMs,budget);
                std::puts("");
                std::fflush(stdout);
            }
            auto& samples=pooled[static_cast<unsigned>(path)];
            samples.insert(samples.end(),times.begin(),times.end());
        };
        constexpr auto a=InputPath::Fixed,b=InputPath::Rotating,c=InputPath::Reuse;
        if(retest){
            for(const auto& order:std::array<std::array<InputPath,3>,6>{{
                {a,b,c},{c,b,a},{b,c,a},{a,c,b},{c,a,b},{b,a,c}}}){
                for(const auto path:order)sample(path);
            }
        }else{
            for(const auto path:{a,c,c,a,c,a,a,c})sample(path);
        }
    }
    for(unsigned i=0;i<pooled.size();++i){
        const auto& t=pooled[i];
        if(!retest&&i==static_cast<unsigned>(InputPath::Rotating))continue;
        Check(t.size()==(retest?6u:4u)*blockFrames,"missing timing samples");
        std::printf("summary %s GPU:   ",PathName(static_cast<InputPath>(i)));
        PrintStats(t,&Sample::gpuMs,budget);
        std::printf("\nsummary %s ready: ",PathName(static_cast<InputPath>(i)));
        PrintStats(t,&Sample::readyMs,budget);
        std::puts("");
    }
    return 0;
}
}
}

int wmain(int argc,wchar_t** argv){
    const auto size=argc>2?std::wstring_view(argv[2]):std::wstring_view{};
    const auto plan=argc==5?std::wstring_view(argv[4]):std::wstring_view{};
    if(argc<4 || argc>5 || (size!=L"fhd"&&size!=L"qhd"&&size!=L"4k") ||
       (std::wstring_view(argv[3])!=L"low"&&std::wstring_view(argv[3])!=L"medium") ||
       (argc==5&&plan!=L"continuous"&&plan!=L"retest-60"&&plan!=L"retest-30")) {
        std::fputs("Usage: VfgInputReuseAudit <runtime-dir> fhd|qhd|4k low|medium [continuous|retest-60|retest-30]\n",stderr);
        return 2;
    }
    const bool uhd=size==L"4k",qhd=size==L"qhd";
    try{return llcv::rife_test::RunAudit(argv[1],uhd?3840:qhd?2560:1920,uhd?2160:qhd?1440:1080,
        std::wstring_view(argv[3])==L"medium",plan==L"continuous"?0:plan==L"retest-30"?30:60,
        plan==L"retest-60"||plan==L"retest-30");}
    catch(const std::exception& e){std::fprintf(stderr,"VFG audit failed: %s\n",e.what());return 1;}
}
