// Private Vulkan layer for the four normalized, planar grids in RIFE 4.25.
#pragma once
#include <pipeline.h>
#include <memory>
#include <string>

class RifeWarp final : public ncnn::Layer {
    std::unique_ptr<ncnn::Pipeline> pipelines[4];
public:
    RifeWarp() { support_vulkan=true; support_fp16_storage=true; }
    int load_param(const ncnn::ParamDict& pd) override {
        return pd.get(0,0)==1 && pd.get(1,0)==2 && pd.get(2,0)==1 && pd.get(3,0)==1 ? 0 : -1;
    }
    int create_pipeline(const ncnn::Option& opt) override {
        static const char shader[]=R"(
layout(binding=0) readonly buffer A{ImageType image[];};
layout(binding=1) readonly buffer B{GridType grid[];};
layout(binding=2) writeonly buffer C{ImageType result[];};
layout(push_constant) uniform parameter {int w,h,c,n,ic,inb,gc,gn,oc,onb;} p;
void main(){
 int x=int(gl_GlobalInvocationID.x), y=int(gl_GlobalInvocationID.y), c=int(gl_GlobalInvocationID.z);
 if(x>=p.w || y>=p.h || c>=p.c)return;
 int i=y*p.w+x;
 for(int b=0;b<p.n;b++){
   int g=b*p.gn+i;
   vec2 uv=vec2(float(grid[g]),float(grid[g+p.gc]));
   uv=mix(uv,vec2(-1),isnan(uv));
   precise vec2 pos=clamp(((uv+1.)*.5)*vec2(p.w-1,p.h-1),vec2(0),vec2(p.w-1,p.h-1));
   ivec2 lo=ivec2(floor(pos)),hi=min(lo+1,ivec2(p.w-1,p.h-1));
   vec2 d=pos-vec2(lo);
   int a=b*p.inb+c*p.ic;
   precise float v=float(image[a+lo.y*p.w+lo.x])*((1.-d.x)*(1.-d.y))
                  +float(image[a+lo.y*p.w+hi.x])*(d.x*(1.-d.y))
                  +float(image[a+hi.y*p.w+lo.x])*((1.-d.x)*d.y)
                  +float(image[a+hi.y*p.w+hi.x])*(d.x*d.y);
   result[b*p.onb+c*p.oc+i]=ImageType(v);
 }
}
)";
        for (int mode=0; mode<(opt.use_fp16_storage?4:1); ++mode) {
            std::string source="#version 450\n";
            if(mode)source+="#extension GL_EXT_shader_16bit_storage : require\n";
            source+=(mode&1)?"#define ImageType float16_t\n":"#define ImageType float\n";
            source+=(mode&2)?"#define GridType float16_t\n":"#define GridType float\n";
            source+=shader;
            std::vector<uint32_t> spv;
            if(ncnn::compile_spirv_module(source.c_str(),opt,spv)!=0)return -1;
            auto& pipeline=pipelines[mode];
            pipeline=std::make_unique<ncnn::Pipeline>(vkdev);
            pipeline->set_local_size_xyz(16,8,1);
            if(pipeline->create(spv.data(),spv.size()*4,{})!=0)return -1;
        }
        return 0;
    }
    int destroy_pipeline(const ncnn::Option&) override {
        for(auto& pipeline:pipelines)pipeline.reset();
        return 0;
    }
    int forward(const std::vector<ncnn::VkMat>& in,std::vector<ncnn::VkMat>& out,
                ncnn::VkCompute& cmd,const ncnn::Option& opt) const override {
        const auto &a=in.at(0), &g=in.at(1);
        if(a.dims!=3||g.dims!=3||a.n!=g.n||a.w!=g.w||a.h!=g.h||g.c!=2||
           a.elempack!=1||g.elempack!=1||
           (a.elemsize!=4&&a.elemsize!=2)||(g.elemsize!=4&&g.elemsize!=2)) {
            std::fprintf(stderr,"warp shape mismatch image %d %d %d %d %d pack%d grid %d %d %d %d %d pack%d\n",a.dims,a.w,a.h,a.c,a.n,a.elempack,g.dims,g.w,g.h,g.c,g.n,g.elempack);
            return -1;
        }
        const auto& pipeline=pipelines[(a.elemsize==2?1:0)|(g.elemsize==2?2:0)];
        if(!pipeline)return -1;
        auto& dst=out.at(0);dst.create_like(a,opt.blob_vkallocator);
        if(dst.empty())return -100;
        int args[]={a.w,a.h,a.c,a.n,int(a.cstep),int(a.nstep),int(g.cstep),int(g.nstep),int(dst.cstep),int(dst.nstep)};
        std::vector<ncnn::vk_constant_type> pc(10);
        for(int i=0;i<10;i++)pc[i].i=args[i];
        cmd.record_pipeline(pipeline.get(),{a,g,dst},pc,dst);
        return 0;
    }
};
DEFINE_LAYER_CREATOR(RifeWarp)
