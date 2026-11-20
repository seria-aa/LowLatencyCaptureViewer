// Fuses channel concatenation with the first block's exact 8:1 bilinear resize.
#pragma once

class RifeInitial final : public ncnn::Layer {
    std::unique_ptr<ncnn::Pipeline> pipeline_;
public:
    RifeInitial() {
        support_vulkan=true; support_fp16_storage=true;
        support_vulkan_packing=true; support_vulkan_any_packing=true;
    }
    int create_pipeline(const ncnn::Option& opt) override {
        if(!opt.use_fp16_storage||opt.use_fp16_arithmetic)return -1;
        static const char shader[]=R"(#version 450
#extension GL_EXT_shader_16bit_storage : require
layout(binding=0) readonly buffer A {float16_t rgb[];};
layout(binding=1) readonly buffer B {float16_t features[];};
layout(binding=2) readonly buffer T {float16_t phase[];};
layout(binding=3) writeonly buffer O {float16_t result[];};
layout(push_constant) uniform parameter {int w,ow,oh,rs,fs,fp,os;} p;
float sampleAt(int at,int c) {
    if(c<6)return float(rgb[c*p.rs+at]);
    if(c==14)return float(phase[at]);
    int feature=c-6;
    return float(features[((feature/p.fp)*p.fs+at)*p.fp+feature%p.fp]);
}
void main(){
    ivec3 id=ivec3(gl_GlobalInvocationID);
    if(id.x>=p.ow||id.y>=p.oh||id.z>=15)return;
    // (output + 0.5) * 8 - 0.5 == output * 8 + 3.5.
    int at=(id.y*8+3)*p.w+id.x*8+3;
    float a=sampleAt(at,id.z)*.5+sampleAt(at+1,id.z)*.5;
    float b=sampleAt(at+p.w,id.z)*.5+sampleAt(at+p.w+1,id.z)*.5;
    result[id.z*p.os+id.y*p.ow+id.x]=float16_t(a*.5+b*.5);
}
)";
        std::vector<uint32_t> spv;
        if(ncnn::compile_spirv_module(shader,opt,spv)!=0)return -1;
        pipeline_=std::make_unique<ncnn::Pipeline>(vkdev);
        pipeline_->set_local_size_xyz(16,4,1);
        return pipeline_->create(spv.data(),spv.size()*4,{});
    }
    int destroy_pipeline(const ncnn::Option&) override {pipeline_.reset();return 0;}
    int forward(const std::vector<ncnn::VkMat>& in,std::vector<ncnn::VkMat>& out,
                ncnn::VkCompute& cmd,const ncnn::Option& opt) const override {
        if(in.size()!=3)return -1;
        const auto &rgb=in[0],&features=in[1],&phase=in[2];
        if(rgb.w<8||rgb.h<8||rgb.w%8||rgb.h%8||rgb.c!=6||rgb.elempack!=1||
            phase.c!=1||phase.elempack!=1||features.c*features.elempack!=8)
            return -1;
        for(const auto& v:in)
            if(v.dims!=3||v.n!=1||v.w!=rgb.w||v.h!=rgb.h||v.elemsize!=2u*v.elempack) {
                std::fprintf(stderr,"unexpected first RIFE block input layout\n");
                return -1;
            }
        auto& dst=out.at(0);
        dst.create(rgb.w/8,rgb.h/8,15,2u,opt.blob_vkallocator);
        if(dst.empty())return -100;
        const int a[]={rgb.w,dst.w,dst.h,int(rgb.cstep),int(features.cstep),features.elempack,int(dst.cstep)};
        std::vector<ncnn::vk_constant_type> pc(7);
        for(int i=0;i<7;++i)pc[i].i=a[i];
        cmd.record_pipeline(pipeline_.get(),{rgb,features,phase,dst},pc,dst);
        return 0;
    }
};
DEFINE_LAYER_CREATOR(RifeInitial)
