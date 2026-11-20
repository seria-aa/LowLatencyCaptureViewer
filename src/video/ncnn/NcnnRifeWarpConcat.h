// Exact sparse taps for the four Half refinement inputs. No model weights.
#pragma once
#include <sstream>
#include <unordered_map>
#include <unordered_set>

class RifeWarpConcat final : public ncnn::Layer {
    std::unique_ptr<ncnn::Pipeline> pipeline_;
    int stride_=0;
    bool half_=false;
    bool vector_=false;
    bool packed_=false;
    bool separate_=false;
public:
    RifeWarpConcat() {support_vulkan=true;support_fp16_storage=true;support_batch=true;}
    int load_param(const ncnn::ParamDict& pd) override {
        stride_=pd.get(0,0);
        vector_=pd.get(1,0)!=0;
        packed_=pd.get(2,0)!=0;
        separate_=pd.get(3,0)!=0;
        support_vulkan_packing=packed_;
        support_vulkan_any_packing=packed_;
        return stride_==1||stride_==2||stride_==4||stride_==8 ? 0 : -1;
    }
    int create_pipeline(const ncnn::Option& opt) override {
        static const char shader[]=R"(
layout(binding=0) readonly buffer A {S source[];};
layout(binding=1) readonly buffer F {S flow[];};
layout(binding=2) readonly buffer T {S time[];};
layout(binding=3) readonly buffer M {S mask[];};
layout(binding=4) readonly buffer L {S latent[];};
layout(binding=5) readonly buffer P {S prev[];};
layout(binding=6) writeonly buffer O {S outputData[];};
#ifdef SPLIT_SOURCE
layout(binding=7) readonly buffer F2 {S features[];};
#endif
layout(push_constant) uniform parameter {
    int w,h,sw,sh,q,sc,sn,fc,fn,lc,pc,oc,lp,pp;
#ifdef SPLIT_SOURCE
    int ec,en,ep;
#endif
} p;
struct Tap {ivec2 a,b;vec2 fraction;};
Tap tapAt(ivec2 x,int group) {
    int i=group*p.fn+x.y*p.w+x.x;
    vec2 mv=vec2(float(flow[i]),float(flow[i+p.fc]));
    mv=mix(mv,vec2(-1e6),isnan(mv));
    vec2 pos=clamp(vec2(x)+mv,vec2(0),vec2(p.w-1,p.h-1));
    ivec2 a=ivec2(pos);
    return Tap(a,min(a+1,ivec2(p.w-1,p.h-1)),pos-vec2(a));
}
float sourceAt(ivec2 x,int group,int plane) {
    int i=x.y*p.w+x.x;
#ifdef SPLIT_SOURCE
    if(plane>=3) {
        int c=plane-3;
        return float(features[(group*p.en+(c/p.ep)*p.ec+i)*p.ep+c%p.ep]);
    }
#endif
    return float(source[group*p.sn+plane*p.sc+i]);
}
float warpedAt(Tap tap,int group,int plane) {
    ivec2 a=tap.a,b=tap.b;
    vec2 d=tap.fraction;
    precise float value=sourceAt(a,group,plane)*((1.-d.x)*(1.-d.y))
                       +sourceAt(ivec2(b.x,a.y),group,plane)*(d.x*(1.-d.y))
                       +sourceAt(ivec2(a.x,b.y),group,plane)*((1.-d.x)*d.y)
                       +sourceAt(b,group,plane)*(d.x*d.y);
    // Match the intermediate storage rounding of the unfused warp.
    return float(S(value));
}
float warped(ivec2 x,int channel) {
    int group=channel<6?channel/3:(channel-6)/4;
    int plane=channel<6?channel%3:3+(channel-6)%4;
    return warpedAt(tapAt(x,group),group,plane);
}
float valueAt(ivec2 x,int c) {
    if(c<14)return warped(x,c);
    int i=x.y*p.w+x.x;
    if(c==14)return float(time[i]);
    if(c==15)return float(mask[i]);
    if(c<24) {
        int k=c-16;
        return float(latent[((k/p.lp)*p.lc+i)*p.lp+k%p.lp]);
    }
    int k=c-24;
    return float(prev[((k/p.pp)*p.pc+i)*p.pp+k%p.pp]);
}
float downsample(ivec2 a,int c) {
    float v=valueAt(a,c);
    if(p.q!=1)
        v=(v+valueAt(a+ivec2(1,0),c)+valueAt(a+ivec2(0,1),c)+
           valueAt(a+ivec2(1,1),c))*.25;
    return v;
}
void storeChannel(int pixel,int c,float v) {
#ifdef PACKED_WARP
    outputData[((c/4)*p.oc+pixel)*4+c%4]=S(v);
#else
    outputData[c*p.oc+pixel]=S(v);
#endif
}
void main(){
    ivec3 o=ivec3(gl_GlobalInvocationID);
    if(o.x>=p.sw||o.y>=p.sh)return;
    ivec2 a=o.xy*p.q+ivec2((p.q-1)/2);
    int dst=o.y*p.sw+o.x;
#ifdef VECTOR_WARP
    if(o.z>=18)return;
    if(o.z<4) {
        int g=o.z%2;
        int c0=o.z<2?g*3:6+4*g;
        int p0=o.z<2?0:3;
        int count=o.z<2?3:4;
        Tap t0=tapAt(a,g);
        Tap t1=t0,t2=t0,t3=t0;
        if(p.q!=1){
            t1=tapAt(a+ivec2(1,0),g);
            t2=tapAt(a+ivec2(0,1),g);
            t3=tapAt(a+ivec2(1,1),g);
        }
        for(int i=0;i<count;++i){
            float v=warpedAt(t0,g,p0+i);
            if(p.q!=1)v=(v+warpedAt(t1,g,p0+i)+warpedAt(t2,g,p0+i)+warpedAt(t3,g,p0+i))*.25;
            storeChannel(dst,c0+i,v);
        }
        return;
    }
    int c=o.z+10;
#else
    if(o.z>=28)return;
    int c=o.z;
#endif
    storeChannel(dst,c,downsample(a,c));
}
)";
        half_=opt.use_fp16_storage;
        std::string src="#version 450\n";
        src+=half_?"#extension GL_EXT_shader_16bit_storage : require\n#define S float16_t\n":"#define S float\n";
        if(packed_)src+="#define PACKED_WARP\n";
        if(vector_)src+="#define VECTOR_WARP\n";
        if(separate_)src+="#define SPLIT_SOURCE\n";
        src+=shader;
        std::vector<uint32_t> spv;
        if(ncnn::compile_spirv_module(src.c_str(),opt,spv)!=0)return -1;
        pipeline_=std::make_unique<ncnn::Pipeline>(vkdev);
        pipeline_->set_local_size_xyz(16,8,1);
        return pipeline_->create(spv.data(),spv.size()*4,{});
    }
    int destroy_pipeline(const ncnn::Option&) override {pipeline_.reset();return 0;}
    int forward(const std::vector<ncnn::VkMat>& in,std::vector<ncnn::VkMat>& out,
                ncnn::VkCompute& cmd,const ncnn::Option& opt) const override {
        if(in.size()!=(separate_?7u:6u))return -1;
        const int channels[]={separate_?3:7,2,1,1,8,4,4};
        const int w=in[0].w,h=in[0].h;
        const size_t size=half_?2u:4u;
        for(size_t i=0;i<in.size();++i) {
            const int pack=in[i].elempack;
            if(in[i].w!=w||in[i].h!=h||(pack!=1&&!(packed_&&i>=4&&(pack==4||pack==8)))||
               in[i].elemsize!=size*pack||in[i].c*pack!=channels[i]||
               in[i].n!=((i<2||i==6)?2:1)) {
                std::fprintf(stderr,"warp concat input %zu: %dx%d c%d n%d pack%d bytes%zu\n",
                    i,in[i].w,in[i].h,in[i].c,in[i].n,in[i].elempack,in[i].elemsize);
                return -1;
            }
        }
        if(w%stride_||h%stride_)return -1;
        auto& dst=out.at(0);
        const int outputPack=packed_?4:1;
        dst.create(w/stride_,h/stride_,28/outputPack,size*outputPack,outputPack,opt.blob_vkallocator);
        if(dst.empty())return -100;
        const int args[]={w,h,dst.w,dst.h,stride_,int(in[0].cstep),int(in[0].nstep),
            int(in[1].cstep),int(in[1].nstep),int(in[4].cstep),int(in[5].cstep),int(dst.cstep),
            in[4].elempack,in[5].elempack};
        std::vector<ncnn::vk_constant_type> pc(separate_?17:14);
        for(int i=0;i<14;++i)pc[i].i=args[i];
        std::vector<ncnn::VkMat> bindings(in.begin(),in.begin()+6);
        bindings.push_back(dst);
        if(separate_) {
            bindings.push_back(in[6]);
            pc[14].i=int(in[6].cstep);
            pc[15].i=int(in[6].nstep);
            pc[16].i=in[6].elempack;
        }
        auto dispatch=dst;
        if(vector_)dispatch.c=18;
        cmd.record_pipeline(pipeline_.get(),bindings,pc,dispatch);
        return 0;
    }
};
DEFINE_LAYER_CREATOR(RifeWarpConcat)

inline std::string FuseRefinementInputs(const std::string& graph,int residual,bool vector,bool packed,
                                       bool initial,bool separate) {
    std::istringstream input(graph);
    std::string magic,counts,line;
    std::getline(input,magic);
    std::getline(input,counts);
    struct Node {std::string type,name,line;std::vector<std::string> src,dst;};
    std::vector<Node> nodes;
    std::unordered_map<std::string,size_t> byName,producer;
    while(std::getline(input,line)) {
        std::istringstream fields(line);
        Node n;
        int src,dst;
        if(!(fields>>n.type>>n.name>>src>>dst))throw std::runtime_error("invalid ncnn layer");
        n.line=line;n.src.resize(src);n.dst.resize(dst);
        for(auto& s:n.src)fields>>s;
        for(auto& s:n.dst){fields>>s;producer.emplace(s,nodes.size());}
        byName.emplace(n.name,nodes.size());
        nodes.push_back(std::move(n));
    }
    auto node=[&](const std::string& name)->Node&{return nodes.at(byName.at(name));};
    auto prev=[&](const std::string& blob)->Node&{return nodes.at(producer.at(blob));};
    std::unordered_set<std::string> skip;
    if(initial) {
        const auto& cat=node("cat_1");
        auto& resize=node("interpolate_63");
        if(cat.type!="Concat"||cat.src.size()!=3||cat.dst.size()!=1||
           cat.line.find(" 0=0")==std::string::npos||resize.type!="Interp"||resize.src!=cat.dst||
           resize.dst.size()!=1||resize.line.find(" 0=2 1=0.125 2=0.125 6=0")==std::string::npos)
            throw std::runtime_error("unexpected first RIFE block");
        int uses=0;
        for(const auto& n:nodes)for(const auto& b:n.src)if(b==cat.dst[0])++uses;
        if(uses!=1)throw std::runtime_error("first concat is still in use");
        resize.line="RifeInitial "+resize.name+" 3 1";
        for(const auto& b:cat.src)resize.line+=" "+b;
        resize.line+=" "+resize.dst[0];
        skip.insert(cat.name);
    }
    const int strides[]={8,4,2,1};
    Node* sourceSplit=nullptr;
    Node* sourceConcat=nullptr;
    std::vector<std::string> rgbBlobs,featureBlobs;
    if(separate) {
        sourceConcat=&node("cat_0");
        sourceSplit=&node("splitncnn_4");
        if(sourceConcat->type!="Concat"||sourceConcat->src.size()!=2||
           sourceConcat->dst.size()!=1||sourceConcat->line.find(" 0=0")==std::string::npos||
           sourceSplit->type!="Split"||sourceSplit->src!=sourceConcat->dst||sourceSplit->dst.size()!=4)
            throw std::runtime_error("unexpected refinement source");
        for(int i=0;i<4;++i) {
            rgbBlobs.push_back("rife_warp_rgb_"+std::to_string(i));
            featureBlobs.push_back("rife_warp_features_"+std::to_string(i));
        }
    }
    for(int i=0;i<4;++i) {
        auto& warp=node("permutegridsample_"+std::to_string(i));
        auto& cat=node("cat_"+std::to_string(i+2));
        auto& gridAdd=prev(warp.src.at(1));
        auto& flowScale=prev(gridAdd.src.at(1));
        const auto& a=prev(cat.src.at(0));
        const auto& b=prev(cat.src.at(1));
        const auto& slice=prev(a.src.at(0));
        if(warp.type!="RifeWarp"||cat.type!="Concat"||cat.src.size()!=6||
           gridAdd.type!="BinaryOp"||flowScale.type!="BinaryOp"||
           a.type!="Reshape"||b.type!="Reshape"||slice.type!="Slice"||
           slice.src.at(0)!=warp.dst.at(0)||prev(b.src.at(0)).name!=slice.name)
            throw std::runtime_error("unexpected Half refinement graph");
        auto& target=i==3?cat:node("interpolate_"+std::to_string(65+2*i));
        if(i!=3&&(target.type!="Interp"||target.src.at(0)!=cat.dst.at(0)))
            throw std::runtime_error("unexpected Half downsample");
        if(separate&&sourceSplit->dst.at(3-i)!=warp.src.at(0))
            throw std::runtime_error("unexpected refinement source order");
        target.line="RifeWarpConcat "+target.name+(separate?" 7 1 "+rgbBlobs[i]:" 6 1 "+warp.src.at(0))+
            " "+flowScale.src.at(0);
        for(int j=2;j<6;++j)target.line+=" "+cat.src.at(j);
        if(separate)target.line+=" "+featureBlobs[i];
        target.line+=" "+target.dst.at(0)+" 0="+std::to_string(strides[i]);
        if(vector)target.line+=" 1=1";
        if(packed)target.line+=" 2=1";
        if(separate)target.line+=" 3=1";
        const Node* removed[]={&warp,&gridAdd,&flowScale,&a,&b,&slice};
        for(const auto* n:removed)skip.insert(n->name);
        if(i!=3)skip.insert(cat.name);
    }
    if(separate) {
        for(const auto& b:sourceSplit->dst) {
            int uses=0;
            for(const auto& n:nodes)for(const auto& v:n.src)if(v==b)++uses;
            if(uses!=1)throw std::runtime_error("refinement source is still in use");
        }
        const std::string rgb=sourceConcat->src[0],features=sourceConcat->src[1];
        sourceConcat->dst=rgbBlobs;
        sourceSplit->dst=featureBlobs;
        sourceConcat->line="Split rife_split_rgb 1 4 "+rgb;
        sourceSplit->line="Split rife_split_features 1 4 "+features;
        for(int i=0;i<4;++i) {
            sourceConcat->line+=" "+rgbBlobs[i];
            sourceSplit->line+=" "+featureBlobs[i];
        }
    }
    if(residual) {
        int folded=0;
        for(auto& n:nodes) {
            if(n.type!="ReLU"||n.line.find(" 0=0.2")==std::string::npos)continue;
            const auto& add=prev(n.src.at(0));
            if(add.type!="BinaryOp"||add.src.size()!=2)continue;
            const auto& mul=prev(add.src.at(0));
            if(mul.type!="BinaryOp"||mul.src.size()!=2)continue;
            const auto& beta=prev(mul.src.at(1));
            if(beta.type!="MemoryData"||beta.name.find(".beta")==std::string::npos)continue;
            if(add.line.rfind(" 0=0")!=add.line.size()-4||mul.line.rfind(" 0=2")!=mul.line.size()-4)
                throw std::runtime_error("unexpected residual arithmetic");
            n.line="RifeResidual "+n.name+" 3 1 "+mul.src[0]+" "+mul.src[1]+" "+add.src[1]+" "+n.dst.at(0);
            skip.insert(add.name);skip.insert(mul.name);++folded;
        }
        if(folded!=residual)throw std::runtime_error("unexpected RIFE residual layer count");
    }
    std::unordered_set<std::string> blobs;
    std::string body;
    size_t layerCount=0;
    for(const auto& n:nodes)if(!skip.count(n.name)) {
        body+=n.line+"\n";++layerCount;
        for(const auto& b:n.dst)blobs.insert(b);
    }
    return magic+"\n"+std::to_string(layerCount)+" "+std::to_string(blobs.size())+"\n"+body;
}
