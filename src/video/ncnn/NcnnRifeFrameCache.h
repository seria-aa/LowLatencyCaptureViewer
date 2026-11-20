// The previous endpoint's four feature planes are reused only after GPU completion.
#pragma once
#include <filesystem>

class RifeFrameCache {
    ncnn::Net encoder_;
    ncnn::VkMat previous_, staged_, packed_;
    ncnn::Pipeline pack_;
    ncnn::VkAllocator* allocator_;

    ncnn::VkMat Encode(ncnn::VkCompute& cmd,const ncnn::VkMat& rgb,
                       ncnn::VkAllocator* scratch=nullptr) {
        auto e=encoder_.create_extractor();
        if(scratch) {
            e.set_blob_vkallocator(scratch);
            e.set_workspace_vkallocator(scratch);
        }
        ncnn::VkMat result;
        Require(e.input("enc_in",rgb)==0&&e.extract("17",result,cmd)==0,"encode endpoint");
        Require(result.n==1&&result.c*result.elempack==4&&
            result.elemsize==2u*result.elempack,"unexpected encoder layout");
        return result;
    }
public:
    RifeFrameCache(const ncnn::VulkanDevice* device,const ncnn::Option& opt,
                   const std::filesystem::path& model):pack_(device),allocator_(opt.blob_vkallocator) {
        encoder_.opt=opt;
        encoder_.set_vulkan_device(device);
        auto param=model, weights=model;
        param+=".ncnn.param";weights+=".ncnn.bin";
        Require(encoder_.load_param(param.c_str())==0&&encoder_.load_model(weights.c_str())==0,"load one-frame encoder");
        for(const auto* layer:encoder_.layers())Require(layer->support_vulkan,"CPU in one-frame encoder");
        static const char shader[]=R"(#version 450
#extension GL_EXT_shader_16bit_storage : require
layout(binding=0) readonly buffer A {float16_t prior[];};
layout(binding=1) readonly buffer B {float16_t current[];};
layout(binding=2) writeonly buffer C {float16_t result[];};
layout(push_constant) uniform parameter {int w,h,c,pack,ac,bc,oc,on;} p;
void main() {
    ivec3 x=ivec3(gl_GlobalInvocationID);
    if(x.x>=p.w||x.y>=p.h||x.z>=p.c)return;
    int i=(x.y*p.w+x.x)*p.pack;
    int a=i+x.z*p.ac*p.pack,b=i+x.z*p.bc*p.pack;
    int dst=i+x.z*p.oc*p.pack;
    for(int k=0;k<p.pack;++k) {
        result[dst+k]=prior[a+k];
        result[dst+p.on*p.pack+k]=current[b+k];
    }
}
)";
        std::vector<uint32_t> spv;
        Require(ncnn::compile_spirv_module(shader,opt,spv)==0,"compile feature pair");
        pack_.set_local_size_xyz(16,8,1);
        Require(pack_.create(spv.data(),spv.size()*4,{})==0,"create feature pair");
    }
    void Prime(ncnn::VkCompute& cmd,const ncnn::VkMat& previousInput) {
        previous_=Encode(cmd,previousInput);
    }
#ifdef LLCV_NCNN_VULKAN_TIMESTAMPS
    // Used only to verify a recorded graph against a different endpoint pair.
    void ReplacePrevious(ncnn::VkCompute& cmd,const ncnn::VkMat& input,
                         const ncnn::Option& opt,ncnn::VkAllocator* scratch) {
        const auto newFeatures=Encode(cmd,input,scratch);
        const auto* address=previous_.data;
        cmd.record_clone(newFeatures,previous_,opt);
        Require(previous_.data==address,"fixed previous feature address changed");
    }
#endif
    const ncnn::VkMat& Advance(ncnn::VkCompute& cmd,const ncnn::VkMat& currentInput) {
        staged_=Encode(cmd,currentInput);
        Require(staged_.w==previous_.w&&staged_.h==previous_.h&&staged_.c==previous_.c&&
            staged_.elempack==previous_.elempack,"endpoint feature layout mismatch");
        packed_.create(staged_.w,staged_.h,staged_.c,staged_.elemsize,staged_.elempack,2,allocator_);
        Require(!packed_.empty(),"allocate feature pair");
        const int values[]={staged_.w,staged_.h,staged_.c,staged_.elempack,int(previous_.cstep),
                            int(staged_.cstep),int(packed_.cstep),int(packed_.nstep)};
        std::vector<ncnn::vk_constant_type> pc(8);
        for(int i=0;i<8;++i)pc[i].i=values[i];
        cmd.record_pipeline(&pack_,{previous_,staged_,packed_},pc,packed_);
        return packed_;
    }
    void Commit() { previous_=std::move(staged_); }
#ifdef LLCV_RIFE_VULKAN_LIVE
    void RecordStableCommit(ncnn::VkCompute& cmd,const ncnn::Option& opt) {
        const auto* storage=previous_.data;
        cmd.record_clone(staged_,previous_,opt);
        Require(previous_.data==storage,"live cache changed recorded buffer");
    }
#endif
};
