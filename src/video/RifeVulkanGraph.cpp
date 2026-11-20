// RIFE 4.25 Fast and Half with FHD synthesis on ncnn Vulkan.
// Only built with LLCV_RIFE_VULKAN. Uses the replay-capable ncnn SDK.
#include <net.h>
#include <gpu.h>
#include <command.h>
#include <layer.h>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>
#include "video/ncnn/NcnnRifeWarp.h"
#include "video/ncnn/NcnnRifeWarpConcat.h"
#include "video/ncnn/NcnnRifeInitial.h"
#include "video/RifeVulkanGraph.h"

#ifndef LLCV_RIFE_VULKAN_LIVE
#define LLCV_RIFE_VULKAN_LIVE
#endif

namespace {
constexpr int W=1920,H=1080;
struct ModelShape {
    int width,height,paddedHeight;
    const char* name;
};
constexpr ModelShape Fast{768,432,448,"fast"};
constexpr ModelShape Half{960,540,544,"half"};
void Require(bool valid,const char* what) { if(!valid) throw std::runtime_error(what); }

#include "video/ncnn/NcnnRifeTail.h"
#include "video/ncnn/NcnnRifeFrameCache.h"

struct Allocators {
    ncnn::Net& net;
    const ncnn::VulkanDevice* device;
    ncnn::VkAllocator* blob;
    ncnn::VkAllocator* staging;
    explicit Allocators(ncnn::Net& n):net(n),device(n.vulkan_device()),
        blob(device->acquire_blob_allocator()),staging(device->acquire_staging_allocator()) {
        net.opt.blob_vkallocator=net.opt.workspace_vkallocator=blob;
        net.opt.staging_vkallocator=staging;
    }
    ~Allocators(){net.clear();device->reclaim_blob_allocator(blob);device->reclaim_staging_allocator(staging);}
};

// Full-resolution warp of both endpoints with the upsampled low flow/mask.
class Synthesis {
    ncnn::Pipeline pipeline_;
    ncnn::VkMat dispatch_;
public:
    Synthesis(const ncnn::VulkanDevice* dev,const ncnn::Option& opt):pipeline_(dev) {
        static const char shader[]=R"(
layout(binding=0) readonly buffer Input {float source[];};
layout(binding=1) readonly buffer FlowIn {float16_t flow[];};
layout(binding=2) readonly buffer MaskIn {float16_t mask[];};
layout(binding=3) writeonly buffer Output {float rgb[];};
layout(push_constant) uniform parameter{int w,h,lw,lh,inputStep,flowStep,flowPack,outputStep;} p;
vec4 getFlow(ivec2 x) {
    int i=x.y*p.lw+x.x;
    return p.flowPack==4 ? vec4(float(flow[i*4]),float(flow[i*4+1]),float(flow[i*4+2]),float(flow[i*4+3])) :
        vec4(float(flow[i]),float(flow[i+p.flowStep]),float(flow[i+2*p.flowStep]),float(flow[i+3*p.flowStep]));
}
float getMask(ivec2 x) { return float(mask[x.y*p.lw+x.x]); }
vec2 lowCoord(ivec2 x) {
    return clamp((vec2(x)+.5)*vec2(p.lw,p.lh)/vec2(p.w,p.h)-.5,vec2(0),vec2(p.lw-1,p.lh-1));
}
vec3 color(ivec2 x,int plane) {
    int i=x.y*p.w+x.x+plane*p.inputStep;
    return vec3(source[i],source[i+p.inputStep],source[i+2*p.inputStep]);
}
vec3 warp(vec2 position,int plane) {
    vec2 uv=clamp(position,vec2(0),vec2(p.w-1,p.h-1));
    ivec2 a=ivec2(uv),b=min(a+1,ivec2(p.w-1,p.h-1));
    vec2 r=uv-vec2(a);
    return mix(mix(color(a,plane),color(ivec2(b.x,a.y),plane),r.x),
               mix(color(ivec2(a.x,b.y),plane),color(b,plane),r.x),r.y);
}
void main() {
    ivec2 x=ivec2(gl_GlobalInvocationID.xy);
    if (x.x>=p.w||x.y>=p.h) return;
    vec2 uv=lowCoord(x);
    ivec2 a=ivec2(uv),b=min(a+1,ivec2(p.lw-1,p.lh-1));
    ivec2 t=ivec2(b.x,a.y),s=ivec2(a.x,b.y);
    vec2 f=uv-vec2(a);
    vec2 scale=vec2(p.w,p.h)/vec2(p.lw,p.lh);
    vec4 mv=mix(mix(getFlow(a),getFlow(t),f.x),mix(getFlow(s),getFlow(b),f.x),f.y)*scale.xyxy;
    float m=mix(mix(getMask(a),getMask(t),f.x),mix(getMask(s),getMask(b),f.x),f.y);
    vec3 value=warp(vec2(x)+mv.xy,0)*m+warp(vec2(x)+mv.zw,3)*(1-m);
    int i=x.y*p.w+x.x;
    rgb[i]=value.r;rgb[i+p.outputStep]=value.g;rgb[i+2*p.outputStep]=value.b;
}
)";
        std::vector<uint32_t> spirv;
        const std::string source=std::string("#version 450\n#extension GL_EXT_shader_16bit_storage : require\n")+shader;
        Require(ncnn::compile_spirv_module(source.c_str(),opt,spirv)==0,"compile Vulkan synthesis");
        pipeline_.set_local_size_xyz(16,8,1);
        Require(pipeline_.create(spirv.data(),spirv.size()*4,{})==0,"create Vulkan synthesis");
    }
    void SetDispatcher(const ncnn::VkMat& value) { dispatch_=value; }
    void Record(ncnn::VkCompute& cmd,const ncnn::VkMat& full,const ncnn::VkMat& flow,
                const ncnn::VkMat& mask,const ncnn::VkMat& output) const {
        Require(full.elempack==1&&full.elemsize==4&&flow.c*flow.elempack==4&&
            (flow.elempack==1||flow.elempack==4)&&flow.elemsize==2u*flow.elempack&&
            mask.c==1&&mask.elempack==1&&mask.elemsize==2u,"unexpected Vulkan synthesis layout");
        const int args[]={W,H,flow.w,flow.h,int(full.cstep),int(flow.cstep),flow.elempack,int(output.cstep)};
        std::vector<ncnn::vk_constant_type> constants(8);
        for(int i=0;i<8;++i) constants[i].i=args[i];
        cmd.record_pipeline(&pipeline_,{full,flow,mask,output},constants,dispatch_);
    }
};

// HUD protection applied to the estimated low-resolution flow: motion is
// scaled by (1 - weight) and the blend is pulled toward 0.5 by weight. The
// weight plane (960x540) comes from the bridge's edge-history detector.
class Protect {
    ncnn::Pipeline pipeline_;
public:
    Protect(const ncnn::VulkanDevice* dev,const ncnn::Option& opt):pipeline_(dev) {
        static const char shader[]=R"(#version 450
#extension GL_EXT_shader_16bit_storage : require
layout(binding=0) buffer FlowIn {float16_t flow[];};
layout(binding=1) buffer MaskIn {float16_t mask[];};
layout(binding=2) readonly buffer Hud {float hud[];};
layout(push_constant) uniform parameter{int lw,lh,uw,uh,weight;} p;
float W(int x,int y){return hud[p.weight+clamp(y,0,p.uh-1)*p.uw+clamp(x,0,p.uw-1)];}
void main() {
    ivec2 x=ivec2(gl_GlobalInvocationID.xy);
    if(x.x>=p.lw||x.y>=p.lh)return;
    vec2 u=clamp((vec2(x)+.5)*vec2(p.uw,p.uh)/vec2(p.lw,p.lh)-.5,vec2(0),vec2(p.uw-1,p.uh-1));
    ivec2 a=ivec2(u);vec2 f=u-vec2(a);
    float w=mix(mix(W(a.x,a.y),W(a.x+1,a.y),f.x),mix(W(a.x,a.y+1),W(a.x+1,a.y+1),f.x),f.y);
    if(w<=0.)return;
    int i=x.y*p.lw+x.x;
    for(int k=0;k<4;++k)flow[i*4+k]=float16_t(float(flow[i*4+k])*(1.-w));
    mask[i]=float16_t(float(mask[i])*(1.-w)+.5*w);
}
)";
        std::vector<uint32_t> spirv;
        Require(ncnn::compile_spirv_module(shader,opt,spirv)==0,"compile HUD protection");
        pipeline_.set_local_size_xyz(16,8,1);
        Require(pipeline_.create(spirv.data(),spirv.size()*4,{})==0,"create HUD protection");
    }
    void Record(ncnn::VkCompute& cmd,const ncnn::VkMat& flow,const ncnn::VkMat& mask,const ncnn::VkMat& hud) const {
        Require(flow.elempack==4&&flow.elemsize==8u&&mask.elempack==1&&mask.elemsize==2u,"unexpected HUD flow layout");
        const int args[]={flow.w,flow.h,llcv::rife_vulkan::kHudWidth,llcv::rife_vulkan::kHudHeight,llcv::rife_vulkan::kHudWeight};
        std::vector<ncnn::vk_constant_type> constants(5);
        for(int i=0;i<5;++i) constants[i].i=args[i];
        cmd.record_pipeline(&pipeline_,{flow,mask,hud},constants,mask);
    }
};

// ncnn-local dispatch pointers (simplevk), not driver hooks. They are only
// swapped for the duration of one device creation or one submission.
PFN_vkCreateDevice plainCreate = nullptr;
llcv::rife_vulkan::ExtendDevice extendCreate = nullptr;
void* createCookie = nullptr;
VkResult VKAPI_CALL ExtendCreate(VkPhysicalDevice physical,const VkDeviceCreateInfo* info,
                                  const VkAllocationCallbacks* allocator,VkDevice* device) {
    return VkResult(extendCreate(createCookie,reinterpret_cast<uintptr_t>(plainCreate),
        physical,info,allocator,device));
}
PFN_vkQueueSubmit plainSubmit = nullptr;
llcv::rife_vulkan::WrappedSubmit wrappedSubmit = nullptr;
void* submitCookie = nullptr;
VkResult VKAPI_CALL ExtendSubmit(VkQueue queue,uint32_t count,const VkSubmitInfo* infos,VkFence fence) {
    return VkResult(wrappedSubmit(submitCookie,reinterpret_cast<uintptr_t>(plainSubmit),queue,
        count,infos,reinterpret_cast<uintptr_t>(fence)));
}
std::mutex g_ncnnLock; // One graph build/submission at a time across owners.

llcv::rife_vulkan::NativeBuffer Expose(const ncnn::VkMat& m) {
    Require(!m.empty()&&m.elempack==1,"Vulkan planar GPU buffer");
    return {reinterpret_cast<uintptr_t>(m.buffer()),m.buffer_offset(),m.cstep*m.c*m.elemsize,int(m.cstep)};
}

class Graph final : public llcv::rife_vulkan::FastGraph {
    const ModelShape shape_;
    ncnn::Net net_;
    std::unique_ptr<Allocators> alloc_;
    ncnn::VkMat full_,low_,endpoint_,output_,flow_,mask_,hud_;
    std::unique_ptr<Protect> protect_;
    std::unique_ptr<Synthesis> synthesis_;
    std::unique_ptr<RifeTail> tail_;
    std::unique_ptr<RifeFrameCache> cache_;
    std::unique_ptr<ncnn::VkCompute> commands_;
    bool submitted_=false;
public:
    Graph(const ncnn::VulkanDevice* device,const std::filesystem::path& models,ModelShape shape,bool hud,bool chroma)
        : shape_(shape) {
        net_.opt.use_vulkan_compute=true;
        net_.opt.use_cooperative_matrix=false;
        net_.opt.use_fp16_packed=net_.opt.use_fp16_storage=true;
        net_.opt.use_fp16_arithmetic=false;
        net_.opt.use_winograd43_convolution=false;
        net_.set_vulkan_device(device);
        const auto stem=std::string(shape_.name);
        std::ifstream params(models/(stem+"-fold-cache.ncnn.param"));
        Require(bool(params),"Vulkan RIFE fold-cache model missing");
        std::string graph,line;
        int warps=0;
        while(std::getline(params,line)) {
            if(line.rfind("GridSample ",0)==0){line.replace(0,10,"RifeWarp");++warps;}
            graph+=line+"\n";
        }
        Require(warps==4&&net_.register_custom_layer("RifeWarp",RifeWarp_layer_creator)==0&&
            net_.register_custom_layer("RifeWarpConcat",RifeWarpConcat_layer_creator)==0&&
            net_.register_custom_layer("RifeInitial",RifeInitial_layer_creator)==0,"Vulkan Fast graph layers");
        graph=FuseRefinementInputs(graph,0,true,true,true,true);
        const auto weights=models/(stem+"-fold-cache.ncnn.bin");
        Require(net_.load_param_mem(graph.c_str())==0&&net_.load_model(weights.c_str())==0,
            "load Vulkan Fast model");
        for(const auto* layer:net_.layers()) Require(layer->support_vulkan,"CPU layer in Vulkan Fast graph");
        alloc_=std::make_unique<Allocators>(net_);
        const int hudFloats=chroma?llcv::rife_vulkan::kHudChromaFloats:llcv::rife_vulkan::kHudFloats;
        {
            ncnn::Mat full(W,H,6),low(shape_.width,shape_.height,7),endpoint(shape_.width,shape_.height,3);
            full.fill(0.f); low.fill(0.f); endpoint.fill(0.f);
            auto fp32=net_.opt;
            fp32.use_fp16_packed=fp32.use_fp16_storage=false;
            ncnn::VkCompute init(device);
            init.record_upload(full,full_,fp32);
            init.record_upload(low,low_,net_.opt);
            init.record_upload(endpoint,endpoint_,net_.opt);
            ncnn::Mat state;
            if(hud){
                // Plain fp32 array: the bridge and Protect index it directly.
                auto plain=fp32;plain.use_packing_layout=false;
                state.create(hudFloats);state.fill(0.f);init.record_upload(state,hud_,plain);
            }
            Require(init.submit_and_wait()==0,"initialize Vulkan frame history");
        }
        if(hud){
            // A 1-D array keeps element order even when ncnn packs it by 4.
            if(!(hud_.dims==1&&hud_.elemsize==size_t(4*hud_.elempack)&&
                 size_t(hud_.w)*size_t(hud_.elempack)>=size_t(hudFloats))){
                char text[160];
                sprintf_s(text,"HUD state layout: dims=%d w=%d h=%d c=%d elemsize=%zu elempack=%d",
                    hud_.dims,hud_.w,hud_.h,hud_.c,hud_.elemsize,hud_.elempack);
                throw std::runtime_error(text);
            }
            protect_=std::make_unique<Protect>(device,net_.opt);
        }
        Require(full_.elemsize==4&&low_.elemsize==2&&endpoint_.elemsize==2,"Vulkan frame precision");
        output_.create(W,H,3,4u,alloc_->blob);
        Require(!output_.empty(),"allocate Vulkan synthesis output");
        synthesis_=std::make_unique<Synthesis>(device,net_.opt);
        auto dispatch=output_; dispatch.c=1;
        synthesis_->SetDispatcher(dispatch);
        tail_=std::make_unique<RifeTail>(device,net_.opt);
        cache_=std::make_unique<RifeFrameCache>(device,net_.opt,models/(stem+"-encoder"));
        {
            ncnn::VkCompute seed(device);
            cache_->Prime(seed,endpoint_);
            Require(seed.submit_and_wait()==0,"initialize Vulkan encoder history");
        }
        commands_=std::make_unique<ncnn::VkCompute>(device,true,true);
    }
    ~Graph() override {
        commands_.reset(); cache_.reset(); tail_.reset(); synthesis_.reset(); protect_.reset();
        full_.release(); low_.release(); endpoint_.release(); output_.release(); flow_.release(); mask_.release(); hud_.release();
        alloc_.reset();
    }
    void Record() override {
        for(auto* input:{&full_,&low_,&endpoint_,&hud_}) {
            if(input->empty())continue;
            input->data->access_flags=VK_ACCESS_SHADER_WRITE_BIT;
            input->data->stage_flags=VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
        }
        const auto& features=cache_->Advance(*commands_,endpoint_);
        auto ex=net_.create_extractor();
        ncnn::VkMat delta,prior;
        Require(ex.input("in0",low_)==0&&ex.input("17",features)==0&&
            ex.extract("413",delta,*commands_)==0&&ex.extract("344",prior,*commands_)==0,
            "record Vulkan RIFE flow");
        tail_->Record(*commands_,delta,prior,flow_,mask_,alloc_->blob,shape_);
        if(protect_)protect_->Record(*commands_,flow_,mask_,hud_);
        synthesis_->Record(*commands_,full_,flow_,mask_,output_);
        cache_->RecordStableCommit(*commands_,net_.opt);
    }
    llcv::rife_vulkan::NativeGraph Native() const override {
        const auto* device=net_.vulkan_device();
        return {ncnn::get_gpu_instance(),device->info.physicalDevice(),device->vkdevice(),
            device->info.compute_queue_family_index(),shape_.width,shape_.height,
            Expose(full_),Expose(low_),Expose(endpoint_),Expose(output_),
            hud_.empty()?llcv::rife_vulkan::NativeBuffer{0,0,0,0}:
                llcv::rife_vulkan::NativeBuffer{reinterpret_cast<uintptr_t>(hud_.buffer()),hud_.buffer_offset(),
                    size_t(hud_.w)*hud_.elemsize,0}};
    }
    std::vector<uint32_t> Compile(const char* source) const override {
        std::vector<uint32_t> words;
        Require(ncnn::compile_spirv_module(source,net_.opt,words)==0&&!words.empty(),"compile Vulkan image bridge");
        return words;
    }
    double Execute(llcv::rife_vulkan::WrappedSubmit submit,void* cookie) override {
        std::lock_guard lock(g_ncnnLock);
        plainSubmit=ncnn::vkQueueSubmit;
        wrappedSubmit=submit;
        submitCookie=cookie;
        struct Restore {~Restore(){ncnn::vkQueueSubmit=plainSubmit;}} restore;
        ncnn::vkQueueSubmit=&ExtendSubmit;
        Require((submitted_?commands_->resubmit_and_wait():commands_->submit_and_wait())==0,"submit Vulkan RIFE frame");
        submitted_=true;
        double gpu=0;
        if(commands_->elapsed_gpu_ms(gpu)!=0)gpu=-1;
        return gpu;
    }
};
} // namespace

namespace llcv::rife_vulkan {
std::unique_ptr<FastGraph> CreateFastGraph(MatchDevice match,ExtendDevice extend,void* cookie,
                                           const std::filesystem::path& modelDirectory,Preset preset,bool hud,bool chroma) {
    std::lock_guard lock(g_ncnnLock);
    Require(ncnn::create_gpu_instance()==0,"create ncnn GPU instance");
    int index=-1;
    for(int i=0;i<ncnn::get_gpu_count();++i) {
        const auto& gpu=ncnn::get_gpu_info(i);
        if(match(cookie,ncnn::get_gpu_instance(),gpu.physicalDevice())){index=i;break;}
    }
    Require(index>=0,"no Vulkan device matches the playback GPU");
    Require(ncnn::get_gpu_info(index).support_fp16_storage(),"Vulkan device lacks FP16 storage");
    plainCreate=ncnn::vkCreateDevice;
    extendCreate=extend;
    createCookie=cookie;
    const ncnn::VulkanDevice* device=nullptr;
    {
        struct Restore {~Restore(){ncnn::vkCreateDevice=plainCreate;}} restore;
        ncnn::vkCreateDevice=&ExtendCreate;
        device=ncnn::get_gpu_device(index);
    }
    Require(device&&device->vkdevice(),"create sharing-enabled ncnn device");
    return std::make_unique<Graph>(device,modelDirectory,preset==Preset::Half?Half:Fast,hud,chroma);
}
} // namespace llcv::rife_vulkan
