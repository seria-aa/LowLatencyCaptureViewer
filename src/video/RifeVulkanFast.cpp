// Vulkan side of the measurement-only RIFE backend. Shares two D3D11
// RGBA8 textures and one D3D11 fence with an ncnn graph on the same adapter.
#define VK_USE_PLATFORM_WIN32_KHR
#define VK_NO_PROTOTYPES
#include "video/RifeVulkanFast.h"
#include "video/RifeVulkanGraph.h"
#include "video/RifeHistoryPolicy.h"
#include <string>
#include <dxgi1_2.h>
#include <vulkan/vulkan.h>
#include <wrl/client.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <exception>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace llcv::rife_vulkan {
namespace {
using Microsoft::WRL::ComPtr;
void Require(bool ok, const char* stage) { if (!ok) throw std::runtime_error(stage); }
int64_t TraceClockUs() {
    using namespace std::chrono;
    return duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
}
void Check(VkResult result, const char* stage) {
    if (result != VK_SUCCESS) {
        char text[200];
        sprintf_s(text, "%s: VkResult %d", stage, int(result));
        throw std::runtime_error(text);
    }
}
void CheckHr(HRESULT result, const char* stage) {
    if (FAILED(result)) {
        char text[200];
        sprintf_s(text, "%s: 0x%08X", stage, unsigned(result));
        throw std::runtime_error(text);
    }
}
struct Handle {
    HANDLE value = nullptr;
    ~Handle() { if (value) CloseHandle(value); }
};
constexpr auto kTextureHandle = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT;
// Older headers call this D3D12_FENCE; it also accepts an ID3D11Fence handle.
constexpr auto kFenceHandle = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT;
constexpr VkImageSubresourceRange kColor{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

struct Loader {
    HMODULE module = nullptr;
    PFN_vkGetInstanceProcAddr instanceAddress = nullptr;
    Loader() {
        module = LoadLibraryExW(L"vulkan-1.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        Require(module != nullptr, "Vulkan loader (vulkan-1.dll) not found");
        instanceAddress = reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(module, "vkGetInstanceProcAddr"));
        Require(instanceAddress != nullptr, "Vulkan loader entry point");
    }
    ~Loader() { if (module) FreeLibrary(module); }
};

struct Sharing {
    Loader& loader;
    LUID luid{};
    static bool Match(void* cookie, void* instance, void* physical) {
        auto& self = *static_cast<Sharing*>(cookie);
        const auto props = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(
            self.loader.instanceAddress(static_cast<VkInstance>(instance), "vkGetPhysicalDeviceProperties2"));
        if (!props) return false;
        VkPhysicalDeviceIDProperties id{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
        VkPhysicalDeviceProperties2 info{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &id};
        props(static_cast<VkPhysicalDevice>(physical), &info);
        return id.deviceLUIDValid && std::memcmp(id.deviceLUID, &self.luid, VK_LUID_SIZE) == 0;
    }
    static int Extend(void*, uintptr_t call, void* physical, const void* source,
                      const void* allocator, void* device) {
        auto info = *static_cast<const VkDeviceCreateInfo*>(source);
        std::vector<const char*> extensions(info.ppEnabledExtensionNames,
            info.ppEnabledExtensionNames + info.enabledExtensionCount);
        for (const char* name : {VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME,
                                 VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME,
                                 VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME}) {
            if (std::none_of(extensions.begin(), extensions.end(),
                             [&](const char* s) { return std::strcmp(s, name) == 0; }))
                extensions.push_back(name);
        }
        const VkPhysicalDeviceTimelineSemaphoreFeatures timeline{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES, const_cast<void*>(info.pNext), VK_TRUE};
        info.pNext = &timeline;
        info.enabledExtensionCount = uint32_t(extensions.size());
        info.ppEnabledExtensionNames = extensions.data();
        return reinterpret_cast<PFN_vkCreateDevice>(call)(static_cast<VkPhysicalDevice>(physical), &info,
            static_cast<const VkAllocationCallbacks*>(allocator), static_cast<VkDevice*>(device));
    }
};

const char* kPackShader = R"(#version 450
#extension GL_EXT_shader_16bit_storage : require
layout(local_size_x=16,local_size_y=8) in;
layout(binding=0,rgba8) readonly uniform image2D rgba;
layout(binding=1,std430) buffer Full {float f[];};
layout(binding=2,std430) buffer Low {float16_t l[];};
layout(binding=3,std430) writeonly buffer End {float16_t e[];};
layout(push_constant) uniform Steps {int fs,ls,es,os,lw,lh,flipY;} p;
vec3 ReadRgb(ivec2 x){return imageLoad(rgba,ivec2(x.x,p.flipY!=0?1079-x.y:x.y)).rgb;}
void main(){
    ivec2 x=ivec2(gl_GlobalInvocationID.xy);
    if(x.x>=1920||x.y>=1080)return;
    int index=x.y*1920+x.x;
    vec3 c=ReadRgb(x);
    for(int k=0;k<3;++k){
        f[index+k*p.fs]=f[index+(k+3)*p.fs];
        f[index+(k+3)*p.fs]=c[k];
    }
    if(x.x>=p.lw||x.y>=p.lh)return;
    vec2 scale=vec2(1920,1080)/vec2(p.lw,p.lh);
    vec2 q=(vec2(x)+.5)*scale-.5;
    ivec2 a=ivec2(q), b=min(a+1,ivec2(1919,1079));
    vec2 t=q-vec2(a);
    c=mix(mix(ReadRgb(a),ReadRgb(ivec2(b.x,a.y)),t.x),
          mix(ReadRgb(ivec2(a.x,b.y)),ReadRgb(b),t.x),t.y);
    int j=x.y*p.lw+x.x;
    for(int k=0;k<3;++k){
        l[j+k*p.ls]=l[j+(k+3)*p.ls];
        l[j+(k+3)*p.ls]=float16_t(c[k]);
        e[j+k*p.es]=float16_t(c[k]);
    }
    l[j+6*p.ls]=float16_t(.5);
}
)";
const char* kUnpackShader = R"(#version 450
layout(local_size_x=16,local_size_y=8) in;
layout(binding=5,rgba8) writeonly uniform image2D rgba;
layout(binding=4,std430) readonly buffer Result {float rgb[];};
layout(push_constant) uniform Steps {int fs,ls,es,os;} p;
void main(){
    ivec2 x=ivec2(gl_GlobalInvocationID.xy);
    if(x.x>=1920||x.y>=1080)return;
    int i=x.y*1920+x.x;
    imageStore(rgba,x,vec4(clamp(vec3(rgb[i],rgb[i+p.os],rgb[i+2*p.os]),0.,1.),1.));
}
)";

// Edge-history HUD detector at 960x540: the same ten steps and thresholds as
// the validated CUDA detector (16-source-pixel transition, radius 8).
constexpr int kHudStages = 10;
std::string HudShader(int stage, bool chroma, HudTrial trial) {
    const int n = kHudPlane;
    std::string s = "#version 450\n#define STAGE " + std::to_string(stage) +
        "\n#define W " + std::to_string(kHudWidth) + "\n#define H " + std::to_string(kHudHeight) +
        "\n#define N " + std::to_string(n) + "\n#define CHROMA " + (chroma ? "1\n" : "0\n") +
        "#define CHROMA_BASE " + std::to_string(kHudFloats) +
        "\n#define TILED " + (trial==HudTrial::Tiled ? "1" : "0") +
        "\n#define SKIP_EMPTY " + (trial==HudTrial::EmptyMask ? "1" : "0") +
        "\n#define EARLY_CHROMA " + (trial==HudTrial::WeakChroma ? "1\n" : "0\n");
    s += R"(
layout(local_size_x=16,local_size_y=8) in;
layout(binding=0,rgba8) readonly uniform image2D rgba;
layout(binding=6,std430) buffer State {float s[];};
layout(binding=7,std430) buffer State4 {vec4 s4[];};
layout(binding=8,std430) buffer Counts {uint u[];};
layout(push_constant) uniform Step {int p,reset;float dt;int radius,flipY;} c;
#define L(k) ((k)*N)
#define LUMA(q) L(q)
#define SMOOTH L(2)
#define DIFF L(3)
#define HOR L(4)
#define ACT L(5)
#define OCC L(6)
#define DIRX L(7)
#define DIRY L(8)
#define DIST L(9)
#define WEIGHT L(10)
#define CAND L(11)
#define CORE L(12)
#define F4(q) ((13+(q)*4)*N/4)
#define STRUCT4 (21*N/4)
#define STRUCTH4 (25*N/4)
#define AGE L(29)
#define COUNT (L(29)+4)
#if SKIP_EMPTY && (STAGE==8 || STAGE==9)
bool HasCore() {
    uint merged=u[COUNT+1];
#if CHROMA
    uint coreCount=float(merged)>.12*N?u[COUNT+3]:merged;
#else
    uint coreCount=merged;
#endif
    return coreCount>0u&&float(coreCount)<=.12*N;
}
#endif
#if CHROMA
#define UV CHROMA_BASE
#define UV_PREV (UV+N)
#define UV_AGE (UV_PREV+N)
#define UV_CAND (UV_AGE+N)
#endif
int I(int x,int y){return clamp(y,0,H-1)*W+clamp(x,0,W-1);}
#if CHROMA
vec2 Chroma(int x,int y){return unpackHalf2x16(u[UV+I(x,y)]);}
#endif
shared uint groupCount;
#if TILED && STAGE==3
shared float activityTile[8][40];
#elif TILED && STAGE==4
shared float activityTile[32][16];
#endif
void Count(bool v,int slot){
    if(gl_LocalInvocationIndex==0)groupCount=0;
    barrier();
    if(v)atomicAdd(groupCount,1u);
    barrier();
    if(gl_LocalInvocationIndex==0&&groupCount>0)atomicAdd(u[COUNT+slot],groupCount);
}
void main(){
    ivec2 g=ivec2(gl_GlobalInvocationID.xy);
    bool inside=g.x<W&&g.y<H;
    int x=g.x,y=g.y,i=y*W+x;
#if STAGE==0
    if(!inside)return;
    float v=0.;
#if CHROMA
    vec2 color=vec2(0.);
#endif
    for(int dy=0;dy<2;++dy)for(int dx=0;dx<2;++dx){
        int row=2*y+dy;
        vec3 q=imageLoad(rgba,ivec2(2*x+dx,c.flipY!=0?1079-row:row)).rgb;
        float y0=q.r*.2126+q.g*.7152+q.b*.0722;
        v+=y0;
#if CHROMA
        color+=q.rb-vec2(y0);
#endif
    }
    s[LUMA(c.p)+i]=v*.25;
#if CHROMA
    u[UV+i]=packHalf2x16(color*.25);
#endif
#elif STAGE==1
    float delta=0.;
    if(inside){
        float sum=0.;
        for(int dy=-1;dy<=1;++dy)for(int dx=-1;dx<=1;++dx)sum+=s[LUMA(c.p)+I(x+dx,y+dy)];
        s[SMOOTH+i]=sum/9.;
        delta=c.reset!=0?0.:abs(s[LUMA(c.p)+i]-s[LUMA(1-c.p)+i]);
        s[DIFF+i]=delta;
    }
    Count(inside&&delta>.12,0);
    Count(inside&&delta>.02,2);
#elif STAGE==2
    if(gl_GlobalInvocationID.x!=0||gl_GlobalInvocationID.y!=0)return;
    bool moving=float(u[COUNT+2])>=.03*N;
    float quiet=c.reset!=0?0.:(moving?0.:s[AGE+1]+c.dt);
    bool clear=c.reset!=0||float(u[COUNT+0])>.70*N||quiet>.5;
    s[AGE+0]=clear?0.:s[AGE+0]+c.dt;s[AGE+1]=clear?0.:quiet;s[AGE+2]=clear?1.:0.;
#elif STAGE==3
#if TILED
    int lx=int(gl_LocalInvocationID.x),ly=int(gl_LocalInvocationID.y);
    for(int j=lx;j<40;j+=16)activityTile[ly][j]=s[DIFF+I(x-lx+j-12,y)];
    barrier();
#endif
    if(!inside)return;
    float sum=0.;
    for(int dx=-12;dx<=12;++dx){
#if TILED
        sum+=activityTile[ly][lx+dx+12];
#else
        sum+=s[DIFF+I(x+dx,y)];
#endif
    }
    s[HOR+i]=sum/25.;
#elif STAGE==4
#if TILED
    int lx=int(gl_LocalInvocationID.x),ly=int(gl_LocalInvocationID.y);
    for(int j=ly;j<32;j+=8)activityTile[j][lx]=s[HOR+I(x,y-ly+j-12)];
    barrier();
#endif
    if(!inside)return;
    float gx=(s[SMOOTH+I(x+1,y)]-s[SMOOTH+I(x-1,y)])*.5;
    float gy=(s[SMOOTH+I(x,y+1)]-s[SMOOTH+I(x,y-1)])*.5;
    float energy=gx*gx+gy*gy,mag=sqrt(energy),cx=(gx*gx-gy*gy)/(energy+1e-8),cy=2.*gx*gy/(energy+1e-8);
    s4[F4(c.p)+i]=vec4(mag,cx,cy,0.);
#if CHROMA
    // Require the color gradient's direction AND polarity to persist.
    // A missed match releases this pixel immediately, including on scene cuts.
    vec4 grad=vec4(Chroma(x+1,y)-Chroma(x-1,y),Chroma(x,y+1)-Chroma(x,y-1))*.5;
    float lenC=length(grad);
#if EARLY_CHROMA
    float colorAge=0.;
    if(lenC>=.035){
        vec4 unitC=grad/lenC;
        if(s[AGE+2]==0.&&dot(unitC,unpackSnorm4x8(u[UV_PREV+i]))>=.94)
            colorAge=min(s[UV_AGE+i]+c.dt,1.);
        u[UV_PREV+i]=packSnorm4x8(unitC);
    }else{
        u[UV_PREV+i]=0u;
    }
#else
    vec4 unitC=lenC>=.035?grad/lenC:vec4(0.);
    bool colorPair=lenC>=.035&&s[AGE+2]==0.&&dot(unitC,unpackSnorm4x8(u[UV_PREV+i]))>=.94;
    float colorAge=colorPair?min(s[UV_AGE+i]+c.dt,1.):0.;
    u[UV_PREV+i]=packSnorm4x8(unitC);
#endif
    s[UV_AGE+i]=colorAge;
    s[UV_CAND+i]=0.;
#endif
    if(s[AGE+2]!=0.){s[OCC+i]=0.;s[DIRX+i]=0.;s[DIRY+i]=0.;s[ACT+i]=0.;s4[STRUCT4+i]=vec4(0.);s[CAND+i]=0.;return;}
    float rate=1.-exp(-c.dt/.55);bool edge=mag>=.012;
    float occ=s[OCC+i]+rate*(float(edge)-s[OCC+i]);
    float mx=s[DIRX+i]+rate*(cx*float(edge)-s[DIRX+i]);
    float my=s[DIRY+i]+rate*(cy*float(edge)-s[DIRY+i]);
    float local=0.;
    for(int dy=-12;dy<=12;++dy){
#if TILED
        local+=activityTile[ly+dy+12][lx];
#else
        local+=s[HOR+I(x,y+dy)];
#endif
    }
    local/=25.;
    float act=s[ACT+i]+rate*(local-s[ACT+i]);
    s[OCC+i]=occ;s[DIRX+i]=mx;s[DIRY+i]=my;s[ACT+i]=act;
    float len=sqrt(mx*mx+my*my);
    bool persistent=occ>=.72&&len/(occ+1e-6)>=.86;
    vec4 old=s4[F4(1-c.p)+i];
    bool pair=edge&&old.x>=.012&&cx*old.y+cy*old.z>=.75;
#if CHROMA
    // Fill gaps in the original detector; never replace a confident luma edge.
    s[UV_CAND+i]=(colorAge>=.75&&mag<.012&&old.x<.012&&act>=.008)?1.:0.;
#endif
    s[CAND+i]=(persistent&&pair&&(mx*cx+my*cy)/(len+1e-6)>=.75&&act>=.008)?1.:0.;
    s4[STRUCT4+i]=persistent?vec4(mx,my,occ,1.):vec4(0.);
#elif STAGE==5
    if(!inside)return;
    vec4 sum=vec4(0.);for(int dx=-5;dx<=5;++dx)sum+=s4[STRUCT4+I(x+dx,y)];s4[STRUCTH4+i]=sum/11.;
#elif STAGE==6
    if(!inside)return;
    vec4 sum=vec4(0.);for(int dy=-5;dy<=5;++dy)sum+=s4[STRUCTH4+I(x,y+dy)];
    float variety=sqrt(sum.x*sum.x+sum.y*sum.y)/(sum.z+11e-6);
    bool keep=s[CAND+i]!=0.&&s[AGE+0]>=1.&&sum.w/11.>=.025&&variety<.94&&float(u[COUNT+2])>=.03*N;
#if CHROMA
    bool color=false;
    if(!keep&&s[UV_CAND+i]!=0.&&s[AGE+0]>=1.&&float(u[COUNT+2])>=.03*N){
        int support=0;
        for(int dy=-1;dy<=1;++dy)for(int dx=-1;dx<=1;++dx){
            if(dx==0&&dy==0)continue;
            int xx=x+dx,yy=y+dy;
            if(xx>=0&&xx<W&&yy>=0&&yy<H&&s[UV_CAND+yy*W+xx]!=0.)++support;
        }
        color=support>=2;
    }
    s[CAND+i]=keep?1.:(color?2.:0.);
#else
    s[CAND+i]=keep?1.:0.;
#endif
#elif STAGE==7
#if CHROMA
    bool luma=false, color=false;
    if(inside)for(int dy=-1;dy<=1;++dy)for(int dx=-1;dx<=1;++dx){
        int xx=x+dx,yy=y+dy;
        if(xx>=0&&xx<W&&yy>=0&&yy<H){
            float v=s[CAND+yy*W+xx]; luma=luma||v==1.; color=color||v==2.;
        }
    }
    if(inside)s[CORE+i]=luma?1.:(color?2.:0.);
    Count(inside&&(luma||color),1);
    barrier();
    Count(inside&&luma,3);
#else
    bool v=false;
    if(inside)for(int dy=-1;dy<=1;++dy)for(int dx=-1;dx<=1;++dx){
        int xx=x+dx,yy=y+dy;if(xx>=0&&xx<W&&yy>=0&&yy<H)v=v||s[CAND+yy*W+xx]!=0.;}
    if(inside)s[CORE+i]=v?1.:0.;
    Count(inside&&v,1);
#endif
#elif STAGE==8
    if(!inside)return;
#if SKIP_EMPTY
    // Stage 9 clears weights on these frames and does not use DIST.
    if(!HasCore())return;
#endif
    int d=c.radius+1;
    for(int step=0;step<=c.radius;++step){
        float left=x>=step?s[CORE+y*W+x-step]:0.;
        float right=x+step<W?s[CORE+y*W+x+step]:0.;
#if CHROMA
        // If color pushes coverage over the cap, fall back to the luma mask.
        if(float(u[COUNT+1])>.12*N){if(left==1.||right==1.){d=step;break;}}
        else
#endif
        if(left!=0.||right!=0.){d=step;break;}
    }
    s[DIST+i]=float(d);
#elif STAGE==9
    if(!inside)return;
#if SKIP_EMPTY
    if(!HasCore()){s[WEIGHT+i]=0.;return;}
#else
#if CHROMA
    if(float(u[COUNT+3])>.12*N){s[WEIGHT+i]=0.;return;}
#else
    if(float(u[COUNT+1])>.12*N){s[WEIGHT+i]=0.;return;}
#endif
#endif
    float dist=float(c.radius+1);
    for(int dy=-c.radius;dy<=c.radius;++dy)if(y+dy>=0&&y+dy<H)dist=min(dist,max(s[DIST+(y+dy)*W+x],float(abs(dy))));
    float t=clamp(1.-dist/float(c.radius),0.,1.);
    s[WEIGHT+i]=t*t*(3.-2.*t);
#endif
}
)";
    return s;
}
} // namespace

struct FastBackend::Impl {
    Loader loader;
    std::unique_ptr<FastGraph> graph;
    ComPtr<ID3D11Texture2D> sharedInput, sharedOutput;
    ComPtr<ID3D11Fence> sharedFence;
    Handle inputReady;
    VkInstance instance = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    PFN_vkGetDeviceProcAddr deviceAddress = nullptr;
    VkImage images[2]{};
    VkDeviceMemory memory[2]{};
    VkImageView views[2]{};
    VkSemaphore timeline = VK_NULL_HANDLE;
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkDescriptorPool descriptors = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    VkPipeline pack = VK_NULL_HANDLE, unpack = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer before = VK_NULL_HANDLE, after = VK_NULL_HANDLE, releaseInput = VK_NULL_HANDLE;
    uint64_t waitValue = 0, signalValue = 0;
    std::wstring name;
    // Kept for re-recording the input commands each job.
    NativeGraph native{};
    VkDescriptorSet bound = VK_NULL_HANDLE;
    int steps[7]{};
    // HUD detector (only when the graph exposes its state buffer).
    bool hud = false, chroma = false;
    HudTrial hudTrial = HudTrial::None;
    bool verifyHud = false;
    bool traceSubmission = false;
    VkQueryPool hudTiming = VK_NULL_HANDLE;
    double timestampNs = 0.;
    VkPipeline detector[kHudStages]{};
    VkBuffer readback = VK_NULL_HANDLE;
    VkDeviceMemory readbackMemory = VK_NULL_HANDLE;
    const uint32_t* readbackData = nullptr;
    int parity = 0;
    int64_t lastArrival = 0, jobArrival = 0;
    uint64_t hudJobs = 0, hudActive = 0;
    double hudCoreSum = 0;
    uint64_t chromaJobs = 0, chromaCapped = 0;
    double chromaCoreSum = 0;

    std::mutex lock;
    std::condition_variable changed;
    bool busy = false, queued = false, finished = false;
    uint64_t queuedWait = 0, queuedSignal = 0;
    int64_t queuedArrival = 0;
    int64_t queuedDeadlineUs = 0;
    int64_t queuedAtUs = 0;
    SubmissionTiming activeTiming{}, reportedTiming{};
    bool queuedFlipY = false;
    JobKind queuedKind = JobKind::Generate, jobKind = JobKind::Generate;
    double lastGpuMs = -1;
    std::exception_ptr error;
    std::atomic<bool> failed{false}, stop{false}, exited{false};
    std::atomic<uint64_t> generatingTicket{0};
    VkFence pendingFence = VK_NULL_HANDLE; // Written by the worker; inspect only once it is idle.
    PFN_vkGetFenceStatus getFenceStatus = nullptr;
    const char* submitFailure = nullptr;
    bool failBeforeSubmitForTest = false;
    bool failAfterSubmitForTest = false;
    bool observeBlockedInputForTest = false;
    std::atomic<bool> blockedInputForTest{false};
    std::thread worker;

    struct Slot {
        std::mutex lock;
        bool active = false;
        // Keep at most one unretired backend out of the render path.
        Impl* retired = nullptr;
        std::chrono::steady_clock::time_point retiredAt{};
        ~Slot() {
            if(retired){
                retired->DrainAtShutdown();
                delete retired;
            }
        }
    };
    static Slot& PlaybackSlot() {
        static Slot slot;
        return slot;
    }
    bool RequestStop() {
        std::lock_guard lk(lock);
        stop.store(true,std::memory_order_release);
        changed.notify_one();
        return !busy;
    }
    bool GpuRetired() {
        if(!pendingFence)return true;
        const auto result=getFenceStatus(device,pendingFence);
        if(result!=VK_SUCCESS&&result!=VK_ERROR_DEVICE_LOST)return false;
        pendingFence=VK_NULL_HANDLE;
        return true;
    }
    void DrainAtShutdown() {
        // The owned VkDevice must retire before ncnn destroys its VkInstance.
        // This is never called from renderer teardown.
        if(worker.joinable())worker.join();
        if(!pendingFence)return;
        const auto wait=D<PFN_vkWaitForFences>("vkWaitForFences");
        while(!GpuRetired()){
            const auto result=wait(device,1,&pendingFence,VK_TRUE,50'000'000);
            if(result==VK_SUCCESS||result==VK_ERROR_DEVICE_LOST){pendingFence=VK_NULL_HANDLE;break;}
            if(result!=VK_TIMEOUT)Sleep(10);
        }
    }
    void RethrowWorkerError() const {
        if(!error)return;
        if(submitFailure)throw std::runtime_error(submitFailure);
        std::rethrow_exception(error);
    }

    template<typename T> T D(const char* fn) const {
        auto value = reinterpret_cast<T>(deviceAddress(device, fn));
        Require(value != nullptr, fn);
        return value;
    }
    template<typename T> T I(const char* fn) const {
        auto value = reinterpret_cast<T>(loader.instanceAddress(instance, fn));
        Require(value != nullptr, fn);
        return value;
    }

    void Import(int index, ID3D11Texture2D* texture, VkPhysicalDevice physical) {
        ComPtr<IDXGIResource1> resource;
        CheckHr(texture->QueryInterface(IID_PPV_ARGS(&resource)), "Vulkan shared texture interface");
        Handle shared;
        CheckHr(resource->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
            nullptr, &shared.value), "Vulkan shared texture handle");
        VkExternalMemoryImageCreateInfo external{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
        external.handleTypes = kTextureHandle;
        VkImageCreateInfo image{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, &external};
        image.imageType = VK_IMAGE_TYPE_2D; image.format = VK_FORMAT_R8G8B8A8_UNORM;
        image.extent = {UINT(kWidth), UINT(kHeight), 1}; image.mipLevels = image.arrayLayers = 1;
        image.samples = VK_SAMPLE_COUNT_1_BIT; image.tiling = VK_IMAGE_TILING_OPTIMAL;
        image.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        Check(D<PFN_vkCreateImage>("vkCreateImage")(device, &image, nullptr, &images[index]), "create Vulkan shared image");
        VkMemoryRequirements requirements{};
        D<PFN_vkGetImageMemoryRequirements>("vkGetImageMemoryRequirements")(device, images[index], &requirements);
        VkMemoryWin32HandlePropertiesKHR handleProps{VK_STRUCTURE_TYPE_MEMORY_WIN32_HANDLE_PROPERTIES_KHR};
        Check(D<PFN_vkGetMemoryWin32HandlePropertiesKHR>("vkGetMemoryWin32HandlePropertiesKHR")
            (device, kTextureHandle, shared.value, &handleProps), "query shared texture memory");
        const auto bits = requirements.memoryTypeBits & handleProps.memoryTypeBits;
        Require(bits != 0, "no compatible Vulkan memory type for shared texture");
        uint32_t type = 0;
        while (!(bits & (uint32_t{1} << type))) ++type;
        (void)physical;
        VkMemoryDedicatedAllocateInfo dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
        dedicated.image = images[index];
        VkImportMemoryWin32HandleInfoKHR importInfo{VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR, &dedicated};
        importInfo.handleType = kTextureHandle; importInfo.handle = shared.value;
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &importInfo};
        allocation.allocationSize = requirements.size; allocation.memoryTypeIndex = type;
        Check(D<PFN_vkAllocateMemory>("vkAllocateMemory")(device, &allocation, nullptr, &memory[index]), "import shared texture");
        Check(D<PFN_vkBindImageMemory>("vkBindImageMemory")(device, images[index], memory[index], 0), "bind shared texture");
        const VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, nullptr, 0, images[index],
            VK_IMAGE_VIEW_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, {}, kColor};
        Check(D<PFN_vkCreateImageView>("vkCreateImageView")(device, &view, nullptr, &views[index]), "shared texture view");
    }

    VkPipeline Pipeline(const char* text) {
        auto code = graph->Compile(text);
        VkShaderModule module = VK_NULL_HANDLE;
        const VkShaderModuleCreateInfo create{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, nullptr, 0,
            code.size() * sizeof(uint32_t), code.data()};
        Check(D<PFN_vkCreateShaderModule>("vkCreateShaderModule")(device, &create, nullptr, &module), "bridge shader");
        const VkPipelineShaderStageCreateInfo stage{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr,
            0, VK_SHADER_STAGE_COMPUTE_BIT, module, "main", nullptr};
        const VkComputePipelineCreateInfo compute{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, nullptr, 0,
            stage, pipelineLayout, VK_NULL_HANDLE, -1};
        VkPipeline result = VK_NULL_HANDLE;
        const auto status = D<PFN_vkCreateComputePipelines>("vkCreateComputePipelines")(device, VK_NULL_HANDLE, 1,
            &compute, nullptr, &result);
        D<PFN_vkDestroyShaderModule>("vkDestroyShaderModule")(device, module, nullptr);
        Check(status, "bridge compute pipeline");
        return result;
    }

    void CreateReadback() {
        VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        info.size = verifyHud ? 64 + sizeof(uint32_t)*kHudPlane : 64;
        info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT; info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        Check(D<PFN_vkCreateBuffer>("vkCreateBuffer")(device, &info, nullptr, &readback), "HUD readback buffer");
        VkMemoryRequirements requirements{};
        D<PFN_vkGetBufferMemoryRequirements>("vkGetBufferMemoryRequirements")(device, readback, &requirements);
        VkPhysicalDeviceMemoryProperties props{};
        I<PFN_vkGetPhysicalDeviceMemoryProperties>("vkGetPhysicalDeviceMemoryProperties")(
            static_cast<VkPhysicalDevice>(native.physical), &props);
        constexpr auto wanted = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        uint32_t type = 0;
        while (type < props.memoryTypeCount && (!(requirements.memoryTypeBits & (1u << type)) ||
               (props.memoryTypes[type].propertyFlags & wanted) != wanted)) ++type;
        Require(type < props.memoryTypeCount, "no host-visible memory for HUD readback");
        const VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr, requirements.size, type};
        Check(D<PFN_vkAllocateMemory>("vkAllocateMemory")(device, &allocation, nullptr, &readbackMemory), "HUD readback memory");
        Check(D<PFN_vkBindBufferMemory>("vkBindBufferMemory")(device, readback, readbackMemory, 0), "bind HUD readback");
        void* mapped = nullptr;
        Check(D<PFN_vkMapMemory>("vkMapMemory")(device, readbackMemory, 0, VK_WHOLE_SIZE, 0, &mapped), "map HUD readback");
        readbackData = static_cast<const uint32_t*>(mapped);
    }

    // Input commands: pack the frame and, with HUD protection, run the
    // detector on the same frame. Re-recorded per job for its time step.
    void RecordBefore(int64_t arrival,bool flipY) {
        const auto start = D<PFN_vkBeginCommandBuffer>("vkBeginCommandBuffer");
        const auto end = D<PFN_vkEndCommandBuffer>("vkEndCommandBuffer");
        const auto barrier = D<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier");
        const auto bindPipeline = D<PFN_vkCmdBindPipeline>("vkCmdBindPipeline");
        const auto bindSets = D<PFN_vkCmdBindDescriptorSets>("vkCmdBindDescriptorSets");
        const auto push = D<PFN_vkCmdPushConstants>("vkCmdPushConstants");
        const auto dispatch = D<PFN_vkCmdDispatch>("vkCmdDispatch");
        const VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, nullptr,
            VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
        Check(start(before, &begin), "record input pack");
        const VkImageMemoryBarrier acquire{
            VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, nullptr, 0, VK_ACCESS_SHADER_READ_BIT,
            VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, VK_QUEUE_FAMILY_EXTERNAL, native.computeFamily, images[0], kColor};
        const VkMemoryBarrier history{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr, VK_ACCESS_MEMORY_WRITE_BIT,
            VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT};
        barrier(before, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
            1, &history, 0, nullptr, 1, &acquire);
        bindPipeline(before, VK_PIPELINE_BIND_POINT_COMPUTE, pack);
        bindSets(before, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout, 0, 1, &bound, 0, nullptr);
        steps[6]=flipY?1:0;
        push(before, pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(steps), steps);
        dispatch(before, (kWidth + 15) / 16, (kHeight + 7) / 8, 1);
        if (hud) {
            const bool reset = llcv::rife_test::HudHistoryDiscontinuity(lastArrival, arrival);
            struct { int p, reset; float dt; int radius,flipY; } step{parity, reset ? 1 : 0,
                reset ? 0.f : float(double(arrival - lastArrival) / 1e6), 8,flipY?1:0};
            lastArrival = arrival;
            const VkMemoryBarrier toFill{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr,
                VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT};
            const VkMemoryBarrier toCompute{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr,
                VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT};
            // Counters without any shader increments still come from the fill.
            const VkMemoryBarrier toReaders{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr,
                VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_SHADER_READ_BIT};
            const auto hudBuffer = reinterpret_cast<VkBuffer>(native.hud.handle);
            const VkDeviceSize countOffset = native.hud.offset + VkDeviceSize(29 * kHudPlane + 4) * 4;
            barrier(before, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &toFill, 0, nullptr, 0, nullptr);
            D<PFN_vkCmdFillBuffer>("vkCmdFillBuffer")(before, hudBuffer, countOffset, 16, 0);
            if (hudTiming) {
                D<PFN_vkCmdResetQueryPool>("vkCmdResetQueryPool")(before,hudTiming,0,2);
                D<PFN_vkCmdWriteTimestamp>("vkCmdWriteTimestamp")(before,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,hudTiming,0);
            }
            for (int stage = 0; stage < kHudStages; ++stage) {
                barrier(before, VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &toCompute, 0, nullptr, 0, nullptr);
                bindPipeline(before, VK_PIPELINE_BIND_POINT_COMPUTE, detector[stage]);
                push(before, pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(step), &step);
                if (stage == 2) dispatch(before, 1, 1, 1);
                else dispatch(before, (kHudWidth + 15) / 16, (kHudHeight + 7) / 8, 1);
            }
            if (hudTiming)
                D<PFN_vkCmdWriteTimestamp>("vkCmdWriteTimestamp")(before,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,hudTiming,1);
            constexpr auto computeAndTransfer=VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
            barrier(before, computeAndTransfer, computeAndTransfer, 0, 1, &toReaders, 0, nullptr, 0, nullptr);
            const VkBufferCopy copy{countOffset, 0, 16};
            D<PFN_vkCmdCopyBuffer>("vkCmdCopyBuffer")(before, hudBuffer, readback, 1, &copy);
            if (verifyHud) {
                const VkBufferCopy mask{native.hud.offset + sizeof(uint32_t)*kHudWeight,64,sizeof(uint32_t)*kHudPlane};
                D<PFN_vkCmdCopyBuffer>("vkCmdCopyBuffer")(before,hudBuffer,readback,1,&mask);
            }
            const VkBufferMemoryBarrier toHost{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER, nullptr,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT,
                VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, readback, 0, VK_WHOLE_SIZE};
            barrier(before, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                0, 0, nullptr, 1, &toHost, 0, nullptr);
            parity = 1 - parity;
        }
        Check(end(before), "finish input pack");
    }

    void Build() {
        const uint32_t count = hud ? 9u : 6u;
        VkDescriptorSetLayoutBinding bindings[9]{};
        for (uint32_t i = 0; i < count; ++i)
            bindings[i] = {i, (i == 0 || i == 5) ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                           1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        const VkDescriptorSetLayoutCreateInfo set{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, nullptr, 0, count, bindings};
        Check(D<PFN_vkCreateDescriptorSetLayout>("vkCreateDescriptorSetLayout")(device, &set, nullptr, &setLayout), "bridge layout");
        const VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(steps)};
        const VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, nullptr, 0, 1, &setLayout, 1, &range};
        Check(D<PFN_vkCreatePipelineLayout>("vkCreatePipelineLayout")(device, &layout, nullptr, &pipelineLayout), "bridge pipeline layout");
        const VkDescriptorPoolSize types[]{{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 7}};
        const VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, nullptr, 0, 1, 2, types};
        Check(D<PFN_vkCreateDescriptorPool>("vkCreateDescriptorPool")(device, &poolInfo, nullptr, &descriptors), "bridge descriptors");
        const VkDescriptorSetAllocateInfo allocation{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, nullptr, descriptors, 1, &setLayout};
        Check(D<PFN_vkAllocateDescriptorSets>("vkAllocateDescriptorSets")(device, &allocation, &bound), "bridge descriptor set");
        const VkDescriptorImageInfo input{VK_NULL_HANDLE, views[0], VK_IMAGE_LAYOUT_GENERAL};
        const VkDescriptorImageInfo output{VK_NULL_HANDLE, views[1], VK_IMAGE_LAYOUT_GENERAL};
        VkDescriptorBufferInfo buffers[4]{};
        VkWriteDescriptorSet writes[9]{};
        const NativeBuffer spans[]{native.full, native.low, native.endpoint, native.output};
        writes[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, bound, 0, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &input, nullptr, nullptr};
        for (uint32_t i = 0; i < 4; ++i) {
            buffers[i] = {reinterpret_cast<VkBuffer>(spans[i].handle), spans[i].offset, spans[i].bytes};
            writes[i + 1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, bound, i + 1, 0, 1,
                             VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &buffers[i], nullptr};
        }
        writes[5] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, bound, 5, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &output, nullptr, nullptr};
        const VkDescriptorBufferInfo state{reinterpret_cast<VkBuffer>(native.hud.handle), native.hud.offset, native.hud.bytes};
        for (uint32_t i = 6; i < count; ++i)
            writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, bound, i, 0, 1,
                         VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &state, nullptr};
        D<PFN_vkUpdateDescriptorSets>("vkUpdateDescriptorSets")(device, count, writes, 0, nullptr);
        pack = Pipeline(kPackShader);
        unpack = Pipeline(kUnpackShader);
        if (hud) {
            for (int stage = 0; stage < kHudStages; ++stage) detector[stage] = Pipeline(HudShader(stage,chroma,hudTrial).c_str());
            CreateReadback();
            if (verifyHud) {
                VkPhysicalDeviceProperties props{};
                I<PFN_vkGetPhysicalDeviceProperties>("vkGetPhysicalDeviceProperties")(static_cast<VkPhysicalDevice>(native.physical),&props);
                timestampNs=props.limits.timestampPeriod;
                const VkQueryPoolCreateInfo query{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,nullptr,0,VK_QUERY_TYPE_TIMESTAMP,2,0};
                Check(D<PFN_vkCreateQueryPool>("vkCreateQueryPool")(device,&query,nullptr,&hudTiming),"HUD timestamp queries");
            }
        }

        const VkCommandPoolCreateInfo commandPool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, nullptr,
            VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, native.computeFamily};
        Check(D<PFN_vkCreateCommandPool>("vkCreateCommandPool")(device, &commandPool, nullptr, &pool), "bridge command pool");
        const VkCommandBufferAllocateInfo commandInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr, pool,
            VK_COMMAND_BUFFER_LEVEL_PRIMARY, 3};
        VkCommandBuffer cmds[3]{};
        Check(D<PFN_vkAllocateCommandBuffers>("vkAllocateCommandBuffers")(device, &commandInfo, cmds), "bridge commands");
        before = cmds[0]; after = cmds[1]; releaseInput = cmds[2];
        const VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        const auto start = D<PFN_vkBeginCommandBuffer>("vkBeginCommandBuffer");
        const auto end = D<PFN_vkEndCommandBuffer>("vkEndCommandBuffer");
        const auto barrier = D<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier");
        const auto bindPipeline = D<PFN_vkCmdBindPipeline>("vkCmdBindPipeline");
        const auto bindSets = D<PFN_vkCmdBindDescriptorSets>("vkCmdBindDescriptorSets");
        const auto push = D<PFN_vkCmdPushConstants>("vkCmdPushConstants");
        const auto dispatch = D<PFN_vkCmdDispatch>("vkCmdDispatch");
        const int values[]{native.full.planeStride, native.low.planeStride, native.endpoint.planeStride,
                           native.output.planeStride, native.estimateWidth, native.estimateHeight};
        std::copy(std::begin(values), std::end(values), steps);
        auto run = [&](VkCommandBuffer cmd, VkPipeline pipeline) {
            bindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
            bindSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout, 0, 1, &bound, 0, nullptr);
            push(cmd, pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(steps), steps);
            dispatch(cmd, (kWidth + 15) / 16, (kHeight + 7) / 8, 1);
        };
        (void)barrier;
        // Input commands are recorded per job (RecordBefore).
        // Output: write the midpoint, release both textures back to D3D11.
        Check(start(after, &begin), "record output unpack");
        const VkMemoryBarrier produced{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr,
            VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT};
        const VkImageMemoryBarrier acquireOutput{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, nullptr,
            0, VK_ACCESS_SHADER_WRITE_BIT, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
            VK_QUEUE_FAMILY_EXTERNAL, native.computeFamily, images[1], kColor};
        barrier(after, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
            1, &produced, 0, nullptr, 1, &acquireOutput);
        run(after, unpack);
        const VkImageMemoryBarrier release[]{
            {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, nullptr, VK_ACCESS_SHADER_READ_BIT, 0,
             VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, native.computeFamily, VK_QUEUE_FAMILY_EXTERNAL, images[0], kColor},
            {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, nullptr, VK_ACCESS_SHADER_WRITE_BIT, 0,
             VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, native.computeFamily, VK_QUEUE_FAMILY_EXTERNAL, images[1], kColor}};
        barrier(after, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0,
            0, nullptr, 0, nullptr, 2, release);
        Check(end(after), "finish output unpack");
        // History-only jobs never acquire or write the output texture.
        Check(start(releaseInput, &begin), "record history-only input release");
        barrier(releaseInput, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0,
            0, nullptr, 0, nullptr, 1, release);
        Check(end(releaseInput), "finish history-only input release");
    }

    VkResult WaitForInput() {
        using namespace std::chrono;
        using llcv::rife_test::SharedFenceProgress;
        using llcv::rife_test::SharedFenceState;
        const auto progress=[&] {return SharedFenceProgress(sharedFence->GetCompletedValue(),waitValue);};
        auto state=progress();
        if(state==SharedFenceState::Ready)return VK_SUCCESS;
        if(state==SharedFenceState::DeviceRemoved){
            submitFailure="D3D11 device removed before Vulkan input";
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        // Never leave the Vulkan queue waiting for a D3D signal that may not
        // arrive (e.g. after device removal). Keep the GPU semaphore wait for visibility.
        ResetEvent(inputReady.value);
        if(FAILED(sharedFence->SetEventOnCompletion(waitValue,inputReady.value))){
            submitFailure="could not monitor the D3D11 input fence";
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        if(observeBlockedInputForTest)blockedInputForTest.store(true,std::memory_order_release);
        const auto limit=steady_clock::now()+seconds(1);
        do {
            if(stop.load(std::memory_order_acquire))return VK_TIMEOUT;
            if(WaitForSingleObject(inputReady.value,10)==WAIT_FAILED){
                submitFailure="D3D11 input fence wait failed";
                return VK_ERROR_INITIALIZATION_FAILED;
            }
            state=progress();
            if(state==SharedFenceState::Ready)return VK_SUCCESS;
        } while(state==SharedFenceState::Waiting&&steady_clock::now()<limit);
        submitFailure=state==SharedFenceState::DeviceRemoved
            ?"D3D11 device removed before Vulkan input":"D3D11 input fence did not signal";
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    static int Submit(void* cookie, uintptr_t call, void* queue, uint32_t count, const void* infos, uintptr_t fence) {
        auto& self = *static_cast<Impl*>(cookie);
        const auto& main = *static_cast<const VkSubmitInfo*>(infos);
        if (count != 1 || main.waitSemaphoreCount || main.signalSemaphoreCount || main.pNext || main.commandBufferCount != 1)
            return VK_ERROR_INITIALIZATION_FAILED;
        const VkTimelineSemaphoreSubmitInfo values{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO, nullptr,
            1, &self.waitValue, 1, &self.signalValue};
        const VkCommandBuffer commands[]{self.before, main.pCommandBuffers[0],
            self.jobKind==JobKind::HistoryOnly?self.releaseInput:self.after};
        constexpr VkPipelineStageFlags stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        const VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO, &values, 1, &self.timeline, &stage,
            3, commands, 1, &self.timeline};
        if(self.stop.load(std::memory_order_acquire))return VK_TIMEOUT;
        const auto submitted=reinterpret_cast<VkFence>(fence);
        if(self.traceSubmission)self.activeTiming.submitBeginUs=TraceClockUs();
        const auto result=reinterpret_cast<PFN_vkQueueSubmit>(call)(static_cast<VkQueue>(queue),1,&submit,submitted);
        if(self.traceSubmission){
            self.activeTiming.submitEndUs=TraceClockUs();
            self.activeTiming.submitResult=result;
            std::lock_guard lk(self.lock);
            self.reportedTiming=self.activeTiming;
        }
        if(result==VK_SUCCESS)self.pendingFence=submitted;
        return result;
    }
    static int Wait(void* cookie,uintptr_t call,void* device,uint32_t count,const void* rawFences,uint32_t all,uint64_t) {
        using namespace std::chrono;
        auto& self=*static_cast<Impl*>(cookie);
        const auto* fences=static_cast<const VkFence*>(rawFences);
        if(count!=1||fences[0]!=self.pendingFence)return VK_ERROR_INITIALIZATION_FAILED;
        if(self.failAfterSubmitForTest) {
            self.submitFailure="injected Vulkan failure after submission";
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }
        const auto wait=reinterpret_cast<PFN_vkWaitForFences>(call);
        const auto limit=steady_clock::now()+seconds(1);
        do {
            const auto result=wait(static_cast<VkDevice>(device),count,fences,all,10'000'000);
            if(result!=VK_TIMEOUT){
                if(result==VK_SUCCESS||result==VK_ERROR_DEVICE_LOST)self.pendingFence=VK_NULL_HANDLE;
                return result;
            }
        } while(!self.stop.load(std::memory_order_acquire)&&steady_clock::now()<limit);
        self.submitFailure="Vulkan job did not retire";
        return VK_TIMEOUT;
    }

    void Work() {
        struct PublishExit {
            std::atomic<bool>& flag;
            ~PublishExit() {flag.store(true,std::memory_order_release);}
        } publish{exited};
        for (;;) {
            bool flipY=false;
            int64_t deadlineUs=0;
            {
                std::unique_lock lk(lock);
                changed.wait(lk, [&] { return queued || stop.load(std::memory_order_acquire); });
                if (stop.load(std::memory_order_acquire)) {queued=busy=false;return;}
                waitValue = queuedWait;
                signalValue = queuedSignal;
                jobArrival = queuedArrival;
                deadlineUs=queuedDeadlineUs;
                flipY = queuedFlipY;
                jobKind = queuedKind;
                if(traceSubmission){
                    activeTiming={signalValue,queuedAtUs};
                    activeTiming.kind=jobKind;
                }
                queued = false;
            }
            try {
                Require(!failBeforeSubmitForTest, "injected Vulkan failure before submission");
                RecordBefore(jobArrival,flipY);
                if(traceSubmission)activeTiming.inputWaitBeginUs=TraceClockUs();
                Check(WaitForInput(),"wait for D3D11 input");
                if(traceSubmission)activeTiming.inputReadyUs=TraceClockUs();
                // The frame may have expired while the render thread's D3D11
                // work was still in flight. Keep history but never write a midpoint.
                if(jobKind==JobKind::Generate&&deadlineUs&&TraceClockUs()>=deadlineUs){
                    jobKind=JobKind::HistoryOnly;
                    if(traceSubmission)activeTiming.expiredBeforeGraph=true;
                }
                if(traceSubmission)activeTiming.kind=jobKind;
                if(jobKind==JobKind::Generate)generatingTicket.store(signalValue,std::memory_order_release);
                const double gpu = graph->Execute(jobKind,&Impl::Submit,&Impl::Wait,this);
                std::lock_guard lk(lock);
                if (hud && readbackData) {
                    // counts[1] = HUD core pixels for this frame (after retirement).
                    const auto merged = readbackData[1];
                    const auto luma = chroma ? readbackData[3] : merged;
                    const bool overCap = float(merged)>.12f*kHudPlane;
                    const auto corePixels = float(luma)>.12f*kHudPlane ? 0u : overCap ? luma : merged;
                    const double core = double(corePixels) / double(kHudPlane);
                    ++hudJobs; hudCoreSum += core; if (corePixels) ++hudActive;
                    if (chroma && merged > luma) {
                        if (overCap) ++chromaCapped;
                        else {
                            ++chromaJobs;
                            chromaCoreSum += double(merged-luma)/double(kHudPlane);
                        }
                    }
                }
                lastGpuMs = gpu;
                finished = true;
                busy = false;
            } catch (...) {
                std::lock_guard lk(lock);
                if(traceSubmission&&!activeTiming.submitEndUs)reportedTiming=activeTiming;
                error = std::current_exception();
                busy = false;
                failed.store(true,std::memory_order_release);
                return;
            }
        }
    }

    ~Impl() {
        if (worker.joinable()) {
            // Destruction is reached only once the worker and GPU are safe.
            worker.join();
        }
        if (device && deviceAddress) {
            if (pool) D<PFN_vkDestroyCommandPool>("vkDestroyCommandPool")(device, pool, nullptr);
            for (auto p : {pack, unpack}) if (p) D<PFN_vkDestroyPipeline>("vkDestroyPipeline")(device, p, nullptr);
            for (auto p : detector) if (p) D<PFN_vkDestroyPipeline>("vkDestroyPipeline")(device, p, nullptr);
            if (hudTiming) D<PFN_vkDestroyQueryPool>("vkDestroyQueryPool")(device,hudTiming,nullptr);
            if (readbackMemory) D<PFN_vkFreeMemory>("vkFreeMemory")(device, readbackMemory, nullptr);
            if (readback) D<PFN_vkDestroyBuffer>("vkDestroyBuffer")(device, readback, nullptr);
            if (pipelineLayout) D<PFN_vkDestroyPipelineLayout>("vkDestroyPipelineLayout")(device, pipelineLayout, nullptr);
            if (descriptors) D<PFN_vkDestroyDescriptorPool>("vkDestroyDescriptorPool")(device, descriptors, nullptr);
            if (setLayout) D<PFN_vkDestroyDescriptorSetLayout>("vkDestroyDescriptorSetLayout")(device, setLayout, nullptr);
            if (timeline) D<PFN_vkDestroySemaphore>("vkDestroySemaphore")(device, timeline, nullptr);
            for (int i = 0; i < 2; ++i) {
                if (views[i]) D<PFN_vkDestroyImageView>("vkDestroyImageView")(device, views[i], nullptr);
                if (images[i]) D<PFN_vkDestroyImage>("vkDestroyImage")(device, images[i], nullptr);
                if (memory[i]) D<PFN_vkFreeMemory>("vkFreeMemory")(device, memory[i], nullptr);
            }
        }
        graph.reset();
    }
};

FastBackend::FastBackend(ID3D11Device* d3d, ID3D11Texture2D* input, ID3D11Texture2D* output,
                         ID3D11Fence* fence, const std::filesystem::path& models, Preset preset, bool hud, bool chroma,
                         HudTrial trial, bool verifyHud, bool traceSubmission,std::stop_token startupStop)
    : impl_(std::make_unique<Impl>()) {
    auto& s = *impl_;
    Require(!startupStop.stop_requested(),"Vulkan RIFE initialization cancelled");
    auto& slot=Impl::PlaybackSlot();
    std::lock_guard lease(slot.lock);
    Require(!slot.active,"another Vulkan RIFE backend is active");
    if(slot.retired) {
        if(!slot.retired->exited.load(std::memory_order_acquire)||!slot.retired->GpuRetired()){
            if(std::chrono::steady_clock::now()-slot.retiredAt<std::chrono::milliseconds(250))throw BackendRetiring{};
            throw std::runtime_error("previous Vulkan RIFE job has not retired");
        }
        delete std::exchange(slot.retired,nullptr);
    }
    s.sharedInput=input;
    s.sharedOutput=output;
    s.sharedFence=fence;
    s.inputReady.value=CreateEventW(nullptr,FALSE,FALSE,nullptr);
    Require(s.inputReady.value!=nullptr,"create Vulkan input fence event");
    ComPtr<IDXGIDevice> dxgi;
    ComPtr<IDXGIAdapter> adapter;
    CheckHr(d3d->QueryInterface(IID_PPV_ARGS(&dxgi)), "Vulkan DXGI device");
    CheckHr(dxgi->GetAdapter(&adapter), "Vulkan DXGI adapter");
    DXGI_ADAPTER_DESC identity{};
    CheckHr(adapter->GetDesc(&identity), "Vulkan adapter identity");
    s.name = identity.Description;
    Sharing sharing{s.loader, identity.AdapterLuid};
    s.graph = CreateFastGraph(&Sharing::Match,&Sharing::Extend,&sharing,models,preset,hud,chroma,startupStop,
        trial==HudTrial::StartupDeviceLoss);
    const auto native = s.graph->Native();
    s.native = native;
    s.hud = native.hud.handle != 0;
    s.chroma = chroma;
    s.hudTrial = trial;
    s.verifyHud = verifyHud;
    s.traceSubmission = traceSubmission;
    s.instance = static_cast<VkInstance>(native.instance);
    s.device = static_cast<VkDevice>(native.device);
    s.deviceAddress = s.I<PFN_vkGetDeviceProcAddr>("vkGetDeviceProcAddr");
    s.getFenceStatus = s.D<PFN_vkGetFenceStatus>("vkGetFenceStatus");
    s.Import(0, input, static_cast<VkPhysicalDevice>(native.physical));
    s.Import(1, output, static_cast<VkPhysicalDevice>(native.physical));

    VkSemaphoreTypeCreateInfo timelineType{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
    timelineType.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    const VkSemaphoreCreateInfo semaphore{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &timelineType};
    Check(s.D<PFN_vkCreateSemaphore>("vkCreateSemaphore")(s.device, &semaphore, nullptr, &s.timeline), "Vulkan timeline");
    Handle fenceHandle;
    CheckHr(fence->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &fenceHandle.value), "export D3D11 fence");
    VkImportSemaphoreWin32HandleInfoKHR importFence{VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_WIN32_HANDLE_INFO_KHR};
    importFence.semaphore = s.timeline; importFence.handleType = kFenceHandle; importFence.handle = fenceHandle.value;
    Check(s.D<PFN_vkImportSemaphoreWin32HandleKHR>("vkImportSemaphoreWin32HandleKHR")(s.device, &importFence),
        "import D3D11 fence");

    Require(!startupStop.stop_requested(),"Vulkan RIFE initialization cancelled");
    s.Build();
    Require(!startupStop.stop_requested(),"Vulkan RIFE initialization cancelled");
    s.graph->Record();
    Require(!startupStop.stop_requested(),"Vulkan RIFE initialization cancelled");
    s.worker = std::thread([&s] { s.Work(); });
    slot.active=true;
}

FastBackend::~FastBackend() {
    auto& slot=Impl::PlaybackSlot();
    std::lock_guard lease(slot.lock);
    const bool idle=impl_->RequestStop();
    if((idle||impl_->exited.load(std::memory_order_acquire))&&impl_->GpuRetired())impl_.reset();
    else {
        slot.retired=impl_.release();
        slot.retiredAt=std::chrono::steady_clock::now();
    }
    slot.active=false;
}

std::future<std::unique_ptr<FastBackend>> FastBackend::TryInitializeAsync(
    ID3D11Device* device, ID3D11Texture2D* input, ID3D11Texture2D* output, ID3D11Fence* fence,
    std::filesystem::path models, Preset preset, bool hud, bool chroma, bool traceSubmission,std::stop_source cancel) {
    // Initialize the playback slot first so it outlives this worker at exit.
    (void)Impl::PlaybackSlot();
    struct LoaderSlot {
        std::mutex lock;
        std::atomic<bool> done{true};
        std::thread worker;
        std::stop_source cancel{std::nostopstate};
        ~LoaderSlot() {if(worker.joinable()){cancel.request_stop();worker.join();}}
    };
    static LoaderSlot loader;
    std::lock_guard guard(loader.lock);
    if(!loader.done.load(std::memory_order_acquire))return {};
    if(loader.worker.joinable())loader.worker.join();
    std::packaged_task<std::unique_ptr<FastBackend>()> task(
        [device=ComPtr<ID3D11Device>(device),input=ComPtr<ID3D11Texture2D>(input),
         output=ComPtr<ID3D11Texture2D>(output),fence=ComPtr<ID3D11Fence>(fence),
         models=std::move(models),preset,hud,chroma,traceSubmission,stop=cancel.get_token()] {
            return std::make_unique<FastBackend>(device.Get(),input.Get(),output.Get(),fence.Get(),
                models,preset,hud,chroma,HudTrial::None,false,traceSubmission,stop);
        });
    auto result=task.get_future();
    loader.cancel=std::move(cancel);
    loader.done.store(false,std::memory_order_release);
    try{
        loader.worker=std::thread([task=std::move(task),done=&loader.done]() mutable {
            {
                // Release the result here if the renderer cancelled its future.
                auto build=std::move(task);
                build();
            }
            done->store(true,std::memory_order_release);
        });
    }catch(...){
        loader.done.store(true,std::memory_order_release);
        throw;
    }
    return result;
}

bool FastBackend::Free() {
    std::lock_guard lk(impl_->lock);
    impl_->RethrowWorkerError();
    return !impl_->busy;
}

bool FastBackend::HasFailed() const noexcept {
    return impl_->failed.load(std::memory_order_acquire);
}

bool FastBackend::GeneratesOutputFor(uint64_t ticket) const noexcept {
    return ticket!=0&&impl_->generatingTicket.load(std::memory_order_acquire)==ticket;
}

void FastBackend::Enqueue(uint64_t waitValue, uint64_t signalValue, int64_t arrivalUs, bool flipY, JobKind kind,
                          int64_t deadlineUs) {
    {
        std::lock_guard lk(impl_->lock);
        impl_->RethrowWorkerError();
        Require(!impl_->busy, "a second Vulkan job cannot be queued");
        impl_->queued = impl_->busy = true;
        impl_->queuedWait = waitValue;
        impl_->queuedSignal = signalValue;
        impl_->queuedArrival = arrivalUs;
        impl_->queuedDeadlineUs = deadlineUs;
        impl_->queuedFlipY = flipY;
        impl_->queuedKind = kind;
        if(impl_->traceSubmission)impl_->queuedAtUs=TraceClockUs();
    }
    impl_->changed.notify_one();
}

bool FastBackend::HudSummary(uint64_t& jobs, uint64_t& active, double& meanCore,
                             uint64_t& chromaJobs, double& meanChromaCore, uint64_t& chromaCapped) {
    std::lock_guard lk(impl_->lock);
    if (!impl_->hud) return false;
    jobs = impl_->hudJobs; active = impl_->hudActive;
    meanCore = impl_->hudJobs ? impl_->hudCoreSum / double(impl_->hudJobs) : 0;
    chromaJobs = impl_->chromaJobs;
    meanChromaCore = jobs ? impl_->chromaCoreSum / double(jobs) : 0;
    chromaCapped = impl_->chromaCapped;
    return true;
}

bool FastBackend::TakeGpuMs(double& ms) {
    std::lock_guard lk(impl_->lock);
    if (!impl_->finished) return false;
    impl_->finished = false;
    ms = impl_->lastGpuMs;
    return true;
}

bool FastBackend::TakeSubmissionTiming(SubmissionTiming& timing) {
    std::lock_guard lk(impl_->lock);
    if(!impl_->reportedTiming.ticket)return false;
    timing=std::exchange(impl_->reportedTiming,{});
    return true;
}

const std::wstring& FastBackend::DeviceName() const { return impl_->name; }

int FastBackend::CompareHud(const std::filesystem::path& models,const std::filesystem::path& report,HudTrial trial) {
    std::ofstream out(report);
    if(!out)return 2;
    try {
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        CheckHr(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,nullptr,0,
            D3D11_SDK_VERSION,&device,nullptr,&context),"HUD comparison D3D11 device");
        ComPtr<ID3D11Device5> device5;
        ComPtr<ID3D11DeviceContext4> context4;
        CheckHr(device.As(&device5),"HUD comparison D3D11.4 device");
        CheckHr(context.As(&context4),"HUD comparison D3D11.4 context");
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width=kWidth;desc.Height=kHeight;desc.MipLevels=desc.ArraySize=desc.SampleDesc.Count=1;
        desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;desc.Usage=D3D11_USAGE_DEFAULT;
        desc.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS;
        desc.MiscFlags=D3D11_RESOURCE_MISC_SHARED_NTHANDLE|D3D11_RESOURCE_MISC_SHARED;
        ComPtr<ID3D11Texture2D> input,output;
        CheckHr(device->CreateTexture2D(&desc,nullptr,&input),"HUD comparison input");
        CheckHr(device->CreateTexture2D(&desc,nullptr,&output),"HUD comparison output");
        if(trial==HudTrial::WorkerFailure){
            using llcv::rife_test::CheckPreviousVulkanJob;
            const auto createFence=[&] {
                ComPtr<ID3D11Fence> fence;
                CheckHr(device5->CreateFence(0,D3D11_FENCE_FLAG_SHARED,IID_PPV_ARGS(&fence)),"worker failure test fence");
                return fence;
            };
            const auto waitForFailure=[](FastBackend& path) {
                const auto expires=std::chrono::steady_clock::now()+std::chrono::seconds(5);
                while(!path.HasFailed()){
                    Require(std::chrono::steady_clock::now()<expires,"worker did not publish failure");
                    Sleep(1);
                }
            };
            {
                auto fence=createFence();
                bool received=false;
                try{FastBackend path(device.Get(),input.Get(),output.Get(),fence.Get(),models,Preset::Fast,
                        false,false,HudTrial::StartupDeviceLoss);}
                catch(const std::runtime_error& e){
                    received=std::string_view(e.what()).find("VK_ERROR_DEVICE_LOST")!=std::string_view::npos;
                }
                Require(received,"startup swallowed the Vulkan device loss");
                FastBackend retry(device.Get(),input.Get(),output.Get(),fence.Get(),models,Preset::Fast);
                out<<"PASS: Vulkan startup propagates device loss and the fresh device initializes\n";
            }
            {
                auto fence=createFence();
                std::stop_source cancel;
                cancel.request_stop();
                auto task=TryInitializeAsync(device.Get(),input.Get(),output.Get(),fence.Get(),
                    models,Preset::Fast,false,false,false,cancel);
                Require(task.valid()&&task.wait_for(std::chrono::seconds(3))==std::future_status::ready,
                    "cancelled Vulkan startup did not return");
                bool received=false;
                try{(void)task.get();}catch(const std::runtime_error& e){
                    received=std::string_view(e.what()).find("initialization cancelled")!=std::string_view::npos;
                }
                Require(received,"cancelled Vulkan startup produced a backend");
                out<<"PASS: a cancelled startup never publishes a Vulkan backend\n";
            }
            {
                auto fence=createFence();
                FastBackend path(device.Get(),input.Get(),output.Get(),fence.Get(),models,Preset::Fast);
                path.impl_->failBeforeSubmitForTest=true;
                path.Enqueue(1,2);
                waitForFailure(path);
                bool received=false, fenceRead=false;
                try {
                    (void)CheckPreviousVulkanJob(2,[&]{return path.Free();},[&]{
                        fenceRead=true;return fence->GetCompletedValue();
                    });
                }catch(const std::runtime_error& e){
                    received=std::string_view(e.what())=="injected Vulkan failure before submission";
                }
                Require(received&&!fenceRead&&fence->GetCompletedValue()==0,
                    "worker failure was masked by the unsignaled fence");
                out<<"PASS: playback checks the worker error before the unsignaled GPU fence\n";
            }
            {
                auto fence=createFence();
                FastBackend path(device.Get(),input.Get(),output.Get(),fence.Get(),models,Preset::Fast);
                path.Enqueue(1,2);
                waitForFailure(path);
                bool received=false;
                try{(void)path.Free();}
                catch(const std::runtime_error& e){received=std::string_view(e.what())=="D3D11 input fence did not signal";}
                Require(received&&!path.impl_->pendingFence&&fence->GetCompletedValue()==0,
                    "Vulkan submitted against a missing D3D11 input signal");
                out<<"PASS: missing D3D11 input signal times out without Vulkan submission\n";
            }
            {
                using namespace std::chrono;
                auto fence=createFence();
                auto path=std::make_unique<FastBackend>(device.Get(),input.Get(),output.Get(),fence.Get(),models,Preset::Fast);
                path->impl_->observeBlockedInputForTest=true;
                path->Enqueue(1,2);
                const auto expires=steady_clock::now()+seconds(5);
                while(!path->impl_->blockedInputForTest.load(std::memory_order_acquire)){
                    Require(steady_clock::now()<expires&&!path->HasFailed(),"worker did not wait for D3D11 input");
                    Sleep(1);
                }
                const auto start=steady_clock::now();
                path.reset();
                Require(steady_clock::now()-start<milliseconds(500)&&fence->GetCompletedValue()==0,
                    "closing playback blocked on a missing input fence");
                // The stop request is asynchronous; a new session may retry next frame.
                while(true) {
                    try{path=std::make_unique<FastBackend>(device.Get(),input.Get(),output.Get(),fence.Get(),models,Preset::Fast);break;}
                    catch(const BackendRetiring&){Sleep(1);}
                }
                out<<"PASS: closing playback does not wait for a missing D3D11 signal\n";
            }
            {
                auto fence=createFence();
                FastBackend path(device.Get(),input.Get(),output.Get(),fence.Get(),models,Preset::Fast);
                CheckHr(context4->Signal(fence.Get(),1),"submitted failure test input");
                context->Flush();
                path.impl_->failAfterSubmitForTest=true;
                path.Enqueue(1,2,1,false,JobKind::HistoryOnly);
                waitForFailure(path);
                Require(path.impl_->pendingFence!=VK_NULL_HANDLE,
                    "a wait failure cleared a submission before GPU retirement was verified");
                out<<"PASS: failed wait preserves the submitted fence for retirement\n";
            }
            {
                auto fence=createFence();
                std::unique_ptr<FastBackend> path;
                // A reset may discard an unfinished initialization. The next
                // playback session must not adopt that result.
                {
                    auto abandoned=TryInitializeAsync(device.Get(),input.Get(),output.Get(),fence.Get(),
                        models,Preset::Fast,false,false,true);
                    Require(abandoned.valid(),"background initializer did not accept the request");
                }
                const auto expires=std::chrono::steady_clock::now()+std::chrono::seconds(8);
                while(!path){
                    auto loading=TryInitializeAsync(device.Get(),input.Get(),output.Get(),fence.Get(),
                        models,Preset::Fast,false,false,true);
                    if(loading.valid()){
                        Require(loading.wait_until(expires)==std::future_status::ready,"Vulkan initialization timed out");
                        try{path=loading.get();}catch(const BackendRetiring&){}
                    }
                    Require(path||std::chrono::steady_clock::now()<expires,"old Vulkan job never retired");
                    if(!path)Sleep(1);
                }
                out<<"PASS: abandoned background initialization does not prevent a fresh session\n";
                path->impl_->observeBlockedInputForTest=true;
                path->Enqueue(1,2,1,false,JobKind::Generate);
                const auto finishedBy=std::chrono::steady_clock::now()+std::chrono::seconds(5);
                while(!path->impl_->blockedInputForTest.load(std::memory_order_acquire)){
                    Require(std::chrono::steady_clock::now()<finishedBy&&!path->HasFailed(),"timing test did not wait for input");
                    Sleep(1);
                }
                const auto inputRelease=TraceClockUs();
                CheckHr(context4->Signal(fence.Get(),1),"Vulkan recovery input");
                context->Flush();
                while(!path->Free()){
                    Require(std::chrono::steady_clock::now()<finishedBy,"new Vulkan job did not retire");
                    Sleep(1);
                }
                Require(fence->GetCompletedValue()==2&&path->GeneratesOutputFor(2),"new Vulkan job did not generate a midpoint");
                SubmissionTiming t;
                Require(path->TakeSubmissionTiming(t)&&t.ticket==2&&t.kind==JobKind::Generate&&
                    t.submitResult==VK_SUCCESS&&
                    t.enqueuedUs>0&&t.enqueuedUs<=t.inputWaitBeginUs&&t.inputWaitBeginUs<=inputRelease&&
                    inputRelease<=t.inputReadyUs&&t.inputReadyUs<=t.submitBeginUs&&t.submitBeginUs<=t.submitEndUs,
                    "D3D11 input wait and Vulkan submit trace were not separated");
                Require(!path->TakeSubmissionTiming(t),"Vulkan submit timing was reported twice");
                out<<"PASS: a fresh Vulkan backend works after retirement\n";
                out<<"PASS: trace pairs the job ticket with enqueue, input wait and Vulkan submit\n";
                path->impl_->blockedInputForTest.store(false,std::memory_order_release);
                const auto deadline=TraceClockUs()+20'000;
                path->Enqueue(3,4,2,false,JobKind::Generate,deadline);
                const auto expiredTestBy=std::chrono::steady_clock::now()+std::chrono::seconds(5);
                while(TraceClockUs()<deadline||!path->impl_->blockedInputForTest.load(std::memory_order_acquire)){
                    Require(std::chrono::steady_clock::now()<expiredTestBy&&!path->HasFailed(),"deadline test did not wait for input");
                    Sleep(1);
                }
                CheckHr(context4->Signal(fence.Get(),3),"expired Vulkan input");
                context->Flush();
                while(!path->Free()){
                    Require(std::chrono::steady_clock::now()<expiredTestBy,"expired Vulkan job did not retire");
                    Sleep(1);
                }
                Require(fence->GetCompletedValue()==4&&!path->GeneratesOutputFor(4)&&
                    path->TakeSubmissionTiming(t)&&t.ticket==4&&t.kind==JobKind::HistoryOnly&&t.expiredBeforeGraph,
                    "expired Vulkan input generated or exposed a stale midpoint");
                out<<"PASS: expiry during the D3D11 wait updates history without exposing an old midpoint\n";
            }
            return 0;
        }
        const bool orientationTest=trial==HudTrial::InputFlip;
        const bool historyTest=trial==HudTrial::HistoryOnly;
        const bool compareOutput=orientationTest||historyTest;
        ComPtr<ID3D11Texture2D> staging;
        if(compareOutput){
            auto stagingDesc=desc;
            stagingDesc.BindFlags=stagingDesc.MiscFlags=0;
            stagingDesc.Usage=D3D11_USAGE_STAGING;
            stagingDesc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
            CheckHr(device->CreateTexture2D(&stagingDesc,nullptr,&staging),"RIFE RGB orientation readback");
        }
        constexpr int frames=110, warmup=12;
        std::vector<uint32_t> pixels(kWidth*kHeight), reference(size_t(frames)*kHudPlane);
        std::vector<uint32_t> expectedOutput(compareOutput?size_t(kWidth)*kHeight*3:0);
        auto rgba=[](int r,int g,int b){return 0xff000000u|uint32_t(r)|(uint32_t(g)<<8)|(uint32_t(b)<<16);};
        auto percentile=[](const std::vector<double>& sorted,double p){return sorted[size_t(p*(sorted.size()-1)+.5)];};
        for(bool color : {false,true}){
            if(!color&&(trial==HudTrial::WeakChroma||compareOutput))continue;
            for(int round=0;round<(compareOutput?2:4);++round){
                const bool candidate=round==1||round==2;
                const bool flip=orientationTest&&candidate;
                ComPtr<ID3D11Fence> fence;
                CheckHr(device5->CreateFence(0,D3D11_FENCE_FLAG_SHARED,IID_PPV_ARGS(&fence)),"HUD comparison fence");
                FastBackend path(device.Get(),input.Get(),output.Get(),fence.Get(),models,Preset::Fast,true,color,
                        candidate&&!historyTest?trial:HudTrial::None,true);
                auto& state=*path.impl_;
                out<<"mode="<<(color?"chroma":"luma")<<" round="<<round<<" candidate="<<candidate
                   <<" trial="<<static_cast<int>(trial)<<std::endl;
                std::vector<double> times,emptyTimes,activeTimes,seedTimes;
                uint64_t differences=0,nonzero=0,afterCut=0,outputDifferences=0;
                for(int frame=0;frame<frames;++frame){
                    for(int y=0;y<kHeight;++y) for(int x=0;x<kWidth;++x){
                        int gray=(((x+13*frame)/40+y/36)&1)?46:62;
                        if(y>=164&&y<218&&x>=164&&x<880)gray=54;
                        auto pixel=rgba(gray,gray,gray);
                        if(frame<88&&y>=184&&y<210&&x>=176&&x<850&&(x%54<26||y%24<4))
                            pixel=(x<480)?rgba(220,220,220):rgba(254,0,0);
                        if(frame>=75&&frame<81&&x>=560+(frame-75)*30&&x<580+(frame-75)*30&&y>=175&&y<240)
                            pixel=rgba(24,200,210);
                        pixels[size_t(flip?kHeight-1-y:y)*kWidth+x]=pixel;
                    }
                    context->UpdateSubresource(input.Get(),0,nullptr,pixels.data(),kWidth*4,0);
                    const uint64_t value=2ull*frame+1;
                    CheckHr(context4->Signal(fence.Get(),value),"HUD comparison signal");
                    context->Flush();
                    const bool afterSeed=frame==67||frame==76||frame==96;
                    const bool seed=historyTest&&!afterSeed&&(frame%7==2||frame==0||frame==66||frame==75||frame==95);
                    const auto enqueueAt=std::chrono::steady_clock::now();
                    path.Enqueue(value,value+1,10000+int64_t(frame)*16667+(frame>=95?200000:0),flip,
                        seed&&candidate?JobKind::HistoryOnly:JobKind::Generate);
                    const auto expires=std::chrono::steady_clock::now()+std::chrono::seconds(8);
                    while(!path.Free()){
                        Require(std::chrono::steady_clock::now()<expires,"HUD comparison GPU timeout");
                        SwitchToThread();
                    }
                    if(seed&&frame>=warmup)
                        seedTimes.push_back(std::chrono::duration<double,std::milli>(
                            std::chrono::steady_clock::now()-enqueueAt).count());
                    const auto* actual=state.readbackData+16;
                    auto* expected=reference.data()+size_t(frame)*kHudPlane;
                    if(round==0)std::copy_n(actual,kHudPlane,expected);
                    else for(int i=0;i<kHudPlane;++i)differences+=actual[i]!=expected[i];
                    if(compareOutput&&afterSeed){
                        const int image=frame==67?0:frame==76?1:2;
                        CheckHr(context4->Wait(fence.Get(),value+1),"RIFE comparison acquire");
                        context->CopyResource(staging.Get(),output.Get());
                        D3D11_MAPPED_SUBRESOURCE mapped{};
                        CheckHr(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped),"read RIFE comparison output");
                        for(int y=0;y<kHeight;++y){
                            const auto* read=reinterpret_cast<const uint32_t*>(
                                static_cast<const unsigned char*>(mapped.pData)+size_t(y)*mapped.RowPitch);
                            auto* wanted=expectedOutput.data()+(size_t(image)*kHeight+y)*kWidth;
                            if(round==0)std::copy_n(read,kWidth,wanted);
                            else for(int x=0;x<kWidth;++x)outputDifferences+=read[x]!=wanted[x];
                        }
                        context->Unmap(staging.Get(),0);
                    }
                    if(state.readbackData[1])++nonzero;
                    if(frame==95)for(int i=0;i<kHudPlane;++i)afterCut+=actual[i]!=0;
                    uint64_t timestamps[2]{};
                    Check(state.D<PFN_vkGetQueryPoolResults>("vkGetQueryPoolResults")(state.device,state.hudTiming,0,2,
                          sizeof(timestamps),timestamps,sizeof(uint64_t),VK_QUERY_RESULT_64_BIT),"read HUD timestamps");
                    if(frame>=warmup){
                        const double ms=double(timestamps[1]-timestamps[0])*state.timestampNs/1e6;
                        times.push_back(ms);
                        (state.readbackData[1]?activeTimes:emptyTimes).push_back(ms);
                    }
                }
                std::sort(times.begin(),times.end());
                std::sort(emptyTimes.begin(),emptyTimes.end());std::sort(activeTimes.begin(),activeTimes.end());
                out<<"p50_ms="<<percentile(times,.5)<<" p95_ms="<<percentile(times,.95)
                   <<" empty_p50_ms="<<(emptyTimes.empty()?0.:percentile(emptyTimes,.5))
                   <<" active_p50_ms="<<(activeTimes.empty()?0.:percentile(activeTimes,.5))
                   <<" differences="<<differences<<" mask_pixels_compared="<<(round==0?0:uint64_t(frames)*kHudPlane)
                   <<" active_frames="<<nonzero<<" chroma_added_frames="<<state.chromaJobs
                   <<" weights_after_scene_cut="<<afterCut<<std::endl;
                Require(differences==0&&nonzero>0&&afterCut==0,"HUD mask comparison failed");
                if(color)Require(state.chromaJobs>0,"synthetic color HUD was not detected");
                if(!seedTimes.empty()){
                    std::sort(seedTimes.begin(),seedTimes.end());
                    out<<"seed_jobs="<<seedTimes.size()<<" seed_observed_p50_ms="<<percentile(seedTimes,.5)<<std::endl;
                }
                if(compareOutput&&candidate){
                    out<<"output_differences="<<outputDifferences<<" output_pixels_compared="<<expectedOutput.size()<<std::endl;
                    Require(outputDifferences==0,"RIFE comparison output differs");
                }
            }
        }
        out<<"PASS\n";
        return 0;
    }catch(const std::exception& error){
        out<<"FAIL: "<<error.what()<<'\n';
        return 1;
    }
}
} // namespace llcv::rife_vulkan
