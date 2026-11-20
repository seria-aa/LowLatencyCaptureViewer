// Prepare final cropped flow/mask in one dispatch from the last refinement.
#pragma once

class RifeTail {
    ncnn::Pipeline pipeline_;
public:
    RifeTail(const ncnn::VulkanDevice* device,const ncnn::Option& opt):pipeline_(device) {
        static const char shader[]=R"(#version 450
#extension GL_EXT_shader_16bit_storage : require
layout(binding=0) readonly buffer A {float16_t delta[];};
layout(binding=1) readonly buffer B {float16_t prior[];};
layout(binding=2) writeonly buffer C {float16_t flow[];};
layout(binding=3) writeonly buffer D {float16_t mask[];};
layout(push_constant) uniform parameter {int w,h,dc,pc,dp,pp;} p;
int offset(int pixel,int c,int stride,int pack) {
    return ((c/pack)*stride+pixel)*pack+c%pack;
}
void main() {
    ivec2 x=ivec2(gl_GlobalInvocationID.xy);
    if(x.x>=p.w||x.y>=p.h)return;
    int pix=x.y*p.w+x.x, coarse=(x.y/2)*(p.w/2)+x.x/2;
    int phase=(x.y%2)*2+x.x%2;
    for(int c=0;c<4;++c) {
        float d=float(delta[offset(coarse,c*4+phase,p.dc,p.dp)]);
        float v=float(prior[offset(pix,c,p.pc,p.pp)]);
        flow[pix*4+c]=float16_t(v+d);
    }
    float m=float(delta[offset(coarse,16+phase,p.dc,p.dp)]);
    mask[pix]=float16_t(1.0/(1.0+exp(-m)));
}
)";
        std::vector<uint32_t> spv;
        Require(ncnn::compile_spirv_module(shader,opt,spv)==0,"compile final flow/mask");
        pipeline_.set_local_size_xyz(16,8,1);
        Require(pipeline_.create(spv.data(),spv.size()*4,{})==0,"create final flow/mask");
    }
    void Record(ncnn::VkCompute& cmd,const ncnn::VkMat& delta,const ncnn::VkMat& prior,
                ncnn::VkMat& flow,ncnn::VkMat& mask,ncnn::VkAllocator* blob,const ModelShape& shape) const {
        Require(delta.w==shape.width/2&&delta.h==shape.paddedHeight/2&&delta.c*delta.elempack==20&&
            delta.elemsize==2u*delta.elempack&&delta.n==1&&prior.w==shape.width&&prior.h==shape.paddedHeight&&
            prior.c*prior.elempack==4&&prior.elemsize==2u*prior.elempack&&prior.n==1,
            "unexpected final flow/mask layout");
        flow.create(shape.width,shape.height,1,8u,4,blob);
        mask.create(shape.width,shape.height,1,2u,1,blob);
        Require(!flow.empty()&&!mask.empty(),"allocate final flow/mask");
        const int args[]={shape.width,shape.height,int(delta.cstep),int(prior.cstep),delta.elempack,prior.elempack};
        std::vector<ncnn::vk_constant_type> pc(6);
        for(int i=0;i<6;++i)pc[i].i=args[i];
        cmd.record_pipeline(&pipeline_,{delta,prior,flow,mask},pc,mask);
    }
};
