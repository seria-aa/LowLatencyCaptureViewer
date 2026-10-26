// Embedded production shaders, synthetic textures only. No viewer/capture/profile.
#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <DirectXPackedVector.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>
#include "../src/video/CmaaNisUpscaler.h"
#include "../third_party/upscaling/nis/NIS_Config.h"
#include "nis128.h"
#include "nis256.h"

template<class T> using Ptr = Microsoft::WRL::ComPtr<T>;
static void Require(bool ok, const char* message) { if(!ok) throw std::runtime_error(message); }
static void Check(HRESULT hr) {
    if(FAILED(hr)) { char text[40];sprintf_s(text,"D3D error %08X",unsigned(hr));throw std::runtime_error(text); }
}
struct Pixel { float r,g,b,a; };
using Image=std::vector<Pixel>;
struct Gpu {
    Ptr<ID3D11Device> d;
    Ptr<ID3D11DeviceContext> c;
    explicit Gpu(bool warp) {
        D3D_FEATURE_LEVEL level;
        Check(D3D11CreateDevice(nullptr,warp?D3D_DRIVER_TYPE_WARP:D3D_DRIVER_TYPE_HARDWARE,
            nullptr,0,nullptr,0,D3D11_SDK_VERSION,&d,&level,&c));
        Require(level>=D3D_FEATURE_LEVEL_11_0,"shader model 5 required");
    }
    Ptr<ID3D11Texture2D> Texture(UINT w,UINT h,DXGI_FORMAT format,UINT flags,
                                const void* data=nullptr,UINT pitch=0) {
        D3D11_TEXTURE2D_DESC desc{};desc.Width=w;desc.Height=h;desc.Format=format;
        desc.MipLevels=desc.ArraySize=desc.SampleDesc.Count=1;desc.BindFlags=flags;
        D3D11_SUBRESOURCE_DATA initial{data,pitch,0};Ptr<ID3D11Texture2D> t;
        Check(d->CreateTexture2D(&desc,data?&initial:nullptr,&t));return t;
    }
    Ptr<ID3D11ShaderResourceView> View(ID3D11Resource* t) {
        Ptr<ID3D11ShaderResourceView> v;Check(d->CreateShaderResourceView(t,nullptr,&v));return v;
    }
    std::vector<BYTE> Bytes(ID3D11Resource* resource) {
        Ptr<ID3D11Texture2D> texture;Ptr<ID3D11Buffer> buffer;Ptr<ID3D11Resource> staging;
        UINT rows=1,rowBytes=0;
        if(SUCCEEDED(resource->QueryInterface(IID_PPV_ARGS(&texture)))) {
            D3D11_TEXTURE2D_DESC desc;texture->GetDesc(&desc);rows=desc.Height;
            Require(desc.Format==DXGI_FORMAT_R16G16B16A16_FLOAT||desc.Format==DXGI_FORMAT_R32_UINT,"readback format");
            rowBytes=desc.Width*(desc.Format==DXGI_FORMAT_R32_UINT?4:8);
            desc.BindFlags=desc.MiscFlags=0;desc.Usage=D3D11_USAGE_STAGING;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
            Ptr<ID3D11Texture2D> t;Check(d->CreateTexture2D(&desc,nullptr,&t));Check(t.As(&staging));
        } else {
            Check(resource->QueryInterface(IID_PPV_ARGS(&buffer)));D3D11_BUFFER_DESC desc;buffer->GetDesc(&desc);
            rowBytes=desc.ByteWidth;desc.BindFlags=desc.MiscFlags=desc.StructureByteStride=0;
            desc.Usage=D3D11_USAGE_STAGING;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
            Ptr<ID3D11Buffer> b;Check(d->CreateBuffer(&desc,nullptr,&b));Check(b.As(&staging));
        }
        c->CopyResource(staging.Get(),resource);D3D11_MAPPED_SUBRESOURCE map{};
        Check(c->Map(staging.Get(),0,D3D11_MAP_READ,0,&map));std::vector<BYTE> bytes(size_t(rows)*rowBytes);
        for(UINT y=0;y<rows;y++)memcpy(bytes.data()+size_t(y)*rowBytes,static_cast<const BYTE*>(map.pData)+size_t(y)*map.RowPitch,rowBytes);
        c->Unmap(staging.Get(),0);return bytes;
    }
    Image Read(ID3D11Resource* t) {
        auto bytes=Bytes(t);Image result(bytes.size()/8);
        for(size_t i=0;i<result.size();i++) {
            float channels[4];
            for(int k=0;k<4;k++) {
                uint16_t half;memcpy(&half,bytes.data()+i*8+k*2,2);
                channels[k]=DirectX::PackedVector::XMConvertHalfToFloat(half);
            }
            result[i]={channels[0],channels[1],channels[2],channels[3]};
        }
        return result;
    }
};
static Image Pattern(UINT w,UINT h,int kind) {
    Image image(size_t(w)*h);UINT seed=1;
    for(UINT y=0;y<h;y++)for(UINT x=0;x<w;x++) {
        seed=1664525*seed+1013904223;float v=.2f;
        if(kind==1&&x==w-1)v=.95f;
        if(kind==2&&y==h-1)v=.95f;
        if(kind==3)v=((x+y)&1)?1.f:0.f;
        if(kind==4)v=((x+y/3)%9<4)?1.f:0.f;
        if(kind==5)v=float(seed>>24)/255;
        image[size_t(y)*w+x]={v,v,v,1};
    }
    return image;
}
static float Difference(const Image& a,const Image& b,UINT stride,UINT l,UINT t,UINT r,UINT bottom) {
    Require(a.size()==b.size(),"comparison size");float error=0;
    for(UINT y=t;y<bottom;y++)for(UINT x=l;x<r;x++) {
        const auto ap=a[size_t(y)*stride+x],bp=b[size_t(y)*stride+x];
        const float ac[]{ap.r,ap.g,ap.b},bc[]{bp.r,bp.g,bp.b};
        for(int k=0;k<3;k++) {
            Require(std::isfinite(ac[k])&&std::isfinite(bc[k]),"finite output");
            error=std::max(error,std::abs(ac[k]-bc[k]));
        }
    }
    return error;
}
struct Nis {
    Gpu& gpu;Ptr<ID3D11ComputeShader> cs;Ptr<ID3D11ShaderResourceView> coefficients[2];Ptr<ID3D11SamplerState> sampler;
    Nis(Gpu& g,int threads):gpu(g) {
        Check(g.d->CreateComputeShader(threads==128?nis128:nis256,threads==128?sizeof(nis128):sizeof(nis256),nullptr,&cs));
        for(int i=0;i<2;i++) {
            auto t=g.Texture(2,64,DXGI_FORMAT_R32G32B32A32_FLOAT,D3D11_BIND_SHADER_RESOURCE,
                i?static_cast<const void*>(coef_usm):coef_scale,8*sizeof(float));coefficients[i]=g.View(t.Get());
        }
        D3D11_SAMPLER_DESC sd{};sd.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU=sd.AddressV=sd.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;sd.MaxLOD=D3D11_FLOAT32_MAX;
        Check(g.d->CreateSamplerState(&sd,&sampler));
    }
    Image Run(const Image& image,UINT w,UINT h,UINT ow,UINT oh,UINT ox=0,UINT oy=0,bool padding=false) {
        auto& g=gpu;g.c->ClearState();auto input=g.Texture(w,h,DXGI_FORMAT_R32G32B32A32_FLOAT,D3D11_BIND_SHADER_RESOURCE,image.data(),w*sizeof(Pixel));auto iv=g.View(input.Get());
        const UINT tw=ow+ox+(padding?8:0),th=oh+oy+(padding?8:0);
        auto output=g.Texture(tw,th,DXGI_FORMAT_R16G16B16A16_FLOAT,D3D11_BIND_UNORDERED_ACCESS);
        Ptr<ID3D11UnorderedAccessView> u;Check(g.d->CreateUnorderedAccessView(output.Get(),nullptr,&u));
        const float sentinel[4]{-9,-9,-9,-9};g.c->ClearUnorderedAccessViewFloat(u.Get(),sentinel);
        NISConfig config{};Require(NVScalerUpdateConfig(config,.15f,0,0,w,h,w,h,ox,oy,ow,oh,tw,th),"NIS config");
        D3D11_BUFFER_DESC bd{};bd.ByteWidth=sizeof(config);bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
        D3D11_SUBRESOURCE_DATA init{&config,0,0};Ptr<ID3D11Buffer> cb;Check(g.d->CreateBuffer(&bd,&init,&cb));
        ID3D11ShaderResourceView* views[]{iv.Get(),coefficients[0].Get(),coefficients[1].Get()};
        g.c->CSSetShaderResources(0,3,views);g.c->CSSetSamplers(0,1,sampler.GetAddressOf());
        g.c->CSSetConstantBuffers(0,1,cb.GetAddressOf());g.c->CSSetUnorderedAccessViews(0,1,u.GetAddressOf(),nullptr);
        g.c->CSSetShader(cs.Get(),nullptr,0);g.c->Dispatch((ow+31)/32,(oh+23)/24,1);g.c->ClearState();return g.Read(output.Get());
    }
};
namespace llcv::upscaling {
struct PipelineTestAccess {
    static UINT HeadWidth(CmaaPipeline& p) {
        Ptr<ID3D11Resource> r;p.uavs_[5]->GetResource(&r);
        Ptr<ID3D11Texture2D> t;Check(r.As(&t));D3D11_TEXTURE2D_DESC d{};t->GetDesc(&d);return d.Width;
    }
    static void PoisonHeads(Gpu& g,CmaaPipeline& p) {
        const UINT invalid[4]{0x03ffffff,0x03ffffff,0x03ffffff,0x03ffffff};
        g.c->ClearUnorderedAccessViewUint(p.uavs_[5].Get(),invalid);
    }
    static std::vector<UINT> Words(Gpu& g,CmaaPipeline& p,unsigned slot) {
        Ptr<ID3D11Resource> r;p.uavs_[slot]->GetResource(&r);auto bytes=g.Bytes(r.Get());
        std::vector<UINT> result(bytes.size()/4);memcpy(result.data(),bytes.data(),bytes.size());return result;
    }
    static void Capacity(Gpu& g,CmaaPipeline& p,unsigned slot,UINT elements) {
        D3D11_BUFFER_DESC d{};d.ByteWidth=elements*(slot==4?8:4);d.StructureByteStride=slot==4?8:4;
        d.BindFlags=D3D11_BIND_UNORDERED_ACCESS;d.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        Ptr<ID3D11Buffer>b;Check(g.d->CreateBuffer(&d,nullptr,&b));D3D11_UNORDERED_ACCESS_VIEW_DESC u{};
        u.ViewDimension=D3D11_UAV_DIMENSION_BUFFER;u.Buffer.NumElements=elements;
        Check(g.d->CreateUnorderedAccessView(b.Get(),&u,p.uavs_[slot].ReleaseAndGetAddressOf()));
    }
};
}
static Image Cmaa(Gpu& g,llcv::upscaling::CmaaPipeline& p,const Image& image,UINT w,UINT h) {
    g.c->ClearState();g.c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    D3D11_RASTERIZER_DESC rd{};rd.FillMode=D3D11_FILL_SOLID;rd.CullMode=D3D11_CULL_NONE;rd.DepthClipEnable=TRUE;
    Ptr<ID3D11RasterizerState> rs;Check(g.d->CreateRasterizerState(&rd,&rs));g.c->RSSetState(rs.Get());
    auto t=g.Texture(w,h,DXGI_FORMAT_R32G32B32A32_FLOAT,D3D11_BIND_SHADER_RESOURCE,image.data(),w*sizeof(Pixel));auto v=g.View(t.Get());
    auto result=p.Draw(g.c.Get(),v.Get());Ptr<ID3D11Resource> resource;result->GetResource(&resource);
    g.c->ClearState();return g.Read(resource.Get());
}
static void ValidateActiveLists(const std::vector<UINT>& heads,const std::vector<UINT>& items,
                                const std::vector<UINT>& locations,UINT count,UINT width) {
    Require(width>0&&heads.size()%width==0,"head texture dimensions");
    Require(count<=locations.size(),"active location count within capacity");
    std::vector<bool> visitedHeads(heads.size(),false);
    std::vector<UINT> visitedItems(items.size()/2,0);
    for(UINT n=0;n<count;n++) {
        const UINT x=locations[n]>>16,y=locations[n]&0xffff;
        Require(x<width&&y<heads.size()/width,"active quad within head texture");
        const size_t quad=size_t(y)*width+x;
        Require(!visitedHeads[quad],"active locations unique");visitedHeads[quad]=true;
        auto head=heads[quad];Require(head!=0xffffffff,"active quad has blend items");
        while(head!=0xffffffff) {
            const UINT index=head&((1u<<26)-1);Require(index<items.size()/2,"no out-of-range linked item");
            Require(visitedItems[index]==0,"no linked-list cycle or blend item shared by two quads");visitedItems[index]=n+1;
            head=items[2*index];
        }
    }
}
static void ValidLists(Gpu& g,llcv::upscaling::CmaaPipeline& p) {
    using A=llcv::upscaling::PipelineTestAccess;const auto control=A::Words(g,p,6);
    Require(control[4]==0&&control[8]==0&&control[12]==0,"append counters reset after dispatch");
    Require((control[0]&~7u)==0,"known overflow reasons");
    if(control[0]!=0) {
        // The dispatch-args pass erased the original append counts. Do not
        // pretend that checking zero active locations validates overflow lists.
        const auto args=A::Words(g,p,7);
        Require(control[3]==0&&(args[0]==0||args[1]==0),"overflow disables AA apply dispatch");
        return;
    }
    ValidateActiveLists(A::Words(g,p,5),A::Words(g,p,4),A::Words(g,p,3),control[3],A::HeadWidth(p));
}
static void ListValidatorTests() {
    // An invalid *inactive* head must be ignored, but active corruption must fail.
    ValidateActiveLists({0x03ffffff,0},{0xffffffff,0},{1u<<16},1,2);
    ValidateActiveLists({0,0x03ffffff},{0,0},{},0,2);
    auto rejects=[](const std::vector<UINT>& heads,const std::vector<UINT>& items,
                    const std::vector<UINT>& locations,UINT count) {
        bool rejected=false;
        try {ValidateActiveLists(heads,items,locations,count,2);} catch(const std::runtime_error&) {rejected=true;}
        Require(rejected,"active list validator detects corruption");
    };
    rejects({0,0x03ffffff},{0xffffffff,0},{1u<<16},1);
    rejects({0,0},{0,0},{1u<<16},1);
    rejects({0,0},{0xffffffff,0},{2u<<16},1);
    rejects({0,0},{0xffffffff,0},{1u<<16},2);
    rejects({0,0},{0xffffffff,0},{1u<<16,1u<<16},2);
    rejects({0,0},{0xffffffff,0},{0,1u<<16},2);
    rejects({0,1},{2,0,2,0,0xffffffff,0},{0,1u<<16},2); // shared tail
    ValidateActiveLists({0,1},{0xffffffff,0,0xffffffff,0},{0,1u<<16},2,2);
    std::puts("PASS: active-list validator ignores inactive heads and rejects bad index/cycle/location/count/duplicates/shared items");
}
static void Boundaries(Gpu& g) {
    for(int threads:{128,256}) {
        Nis nis(g,threads);
        for(const auto dims:{std::vector<UINT>{96,72,120,90},{96,72,128,96},{96,72,144,108},
                            {96,72,192,144},{97,73,151,113},{96,72,96,144},{96,72,192,72},
                            {1920,1080,2560,1440}}) {
            const auto w=dims[0],h=dims[1],ow=dims[2],oh=dims[3];
            auto flat=nis.Run(Pattern(w,h,0),w,h,ow,oh);
            for(const auto p:flat)Require(std::isfinite(p.r)&&std::abs(p.r-.2f)<.0002f,"flat output fully written");
            auto right=nis.Run(Pattern(w,h,1),w,h,ow,oh),bottom=nis.Run(Pattern(w,h,2),w,h,ow,oh);
            Require(Difference(flat,right,ow,0,0,ow/2,oh)==0,"right edge must not affect left half");
            Require(Difference(flat,bottom,ow,0,0,ow,oh/2)==0,"bottom edge must not affect top half");
        }
        auto padded=nis.Run(Pattern(97,73,0),97,73,151,113,3,4,true);
        for(UINT y=0;y<125;y++)for(UINT x=0;x<162;x++) {
            const bool inside=x>=3&&x<154&&y>=4&&y<117;
            Require(inside?std::abs(padded[y*162+x].r-.2f)<.0002f:padded[y*162+x].r==-9,"viewport sentinel incl last row/column");
        }
    }
    std::puts("PASS: both NIS kernels, opposite-edge independence, odd/one-axis scales, offset viewport bounds");
}
static void Overflow(Gpu& g) {
    using llcv::upscaling::CmaaPipeline;using A=llcv::upscaling::PipelineTestAccess;
    for(const auto size:{std::pair<UINT,UINT>{97,73},{320,180},{1920,1080}}) {
        const UINT w=size.first,h=size.second;CmaaPipeline p;Check(p.Configure(g.d.Get(),w,h));
        auto input=Pattern(w,h,5),result=Cmaa(g,p,input,w,h);auto controls=A::Words(g,p,6);
        Require((controls[0]&2)!=0,"dense noise actually exceeds blend capacity");
        Require(controls[3]==0,"overflow skips whole AA apply");ValidLists(g,p);
        Require(Difference(input,result,w,0,0,w,h)<.001f,"overflow retains original SDR image");
        for(int i=0;i<3;i++)Require(Difference(result,Cmaa(g,p,input,w,h),w,0,0,w,h)==0,"overflow fallback stable");
        auto ordinary=Pattern(w,h,4);auto recovery=Cmaa(g,p,ordinary,w,h);
        Require(A::Words(g,p,6)[0]==0,"overflow does not stick on next frame");
        Require(Difference(ordinary,recovery,w,0,0,w,h)>.01f,"CMAA really resumes (not permanent bypass)");ValidLists(g,p);
        Require(Difference(recovery,Cmaa(g,p,ordinary,w,h),w,0,0,w,h)<=1.f/255,"normal AA repeat rounding bound");
    }
    for(unsigned slot:{2u,3u,4u}) {
        CmaaPipeline p;Check(p.Configure(g.d.Get(),97,73));A::Capacity(g,p,slot,8);
        auto input=Pattern(97,73,3),out=Cmaa(g,p,input,97,73);auto control=A::Words(g,p,6);
        const UINT reason=slot==2?1:slot==3?4:2;
        Require((control[0]&reason)!=0&&control[3]==0,"forced capacity guard and safe dispatch");
        Require(Difference(input,out,97,0,0,97,73)<.001f,"no partial AA on forced overflow");ValidLists(g,p);
    }
    // Ordinary exact-capacity checkerboard must not be treated as overflow.
    CmaaPipeline p;Check(p.Configure(g.d.Get(),97,73));Cmaa(g,p,Pattern(97,73,3),97,73);
    Require(A::Words(g,p,6)[0]==0,"exact capacity is allowed");ValidLists(g,p);
    std::puts("PASS: dense overflow, forced bounds, disabled overflow apply, normal active links, stable fallback, recovery, exact capacity");
}
static void FlatTransitions(Gpu& g) {
    using A=llcv::upscaling::PipelineTestAccess;
    constexpr UINT w=97,h=73;llcv::upscaling::CmaaPipeline p;Check(p.Configure(g.d.Get(),w,h));
    A::PoisonHeads(g,p);
    const auto flat=Pattern(w,h,0);
    auto checkFlat=[&]() {
        const auto output=Cmaa(g,p,flat,w,h);const auto control=A::Words(g,p,6);
        Require(control[0]==0&&control[3]==0,"flat frame has no overflow or active quads");
        Require(Difference(flat,output,w,0,0,w,h)<.001f,"flat frame retains original image");ValidLists(g,p);
        return output;
    };
    const auto first=checkFlat();
    const auto heads=A::Words(g,p,5);
    Require(std::find(heads.begin(),heads.end(),0x03ffffffu)!=heads.end(),"flat test really includes untouched heads");
    for(int kind:{4,5,4,5}) {
        const auto input=Pattern(w,h,kind);Cmaa(g,p,input,w,h);ValidLists(g,p);
        const auto control=A::Words(g,p,6);
        Require(kind==5?(control[0]&2)!=0:control[0]==0,"transition exercises normal and overflow frames");
        Require(Difference(first,checkFlat(),w,0,0,w,h)==0,"complex to flat leaves no previous-frame residue");
    }
    std::puts("PASS: poisoned inactive heads, first flat frame, normal/overflow -> flat -> normal transitions");
}
static void OverflowSequence(Gpu& g) {
    using A=llcv::upscaling::PipelineTestAccess;
    constexpr UINT w=320,h=180,ow=480,oh=270;
    llcv::upscaling::CmaaPipeline p;Check(p.Configure(g.d.Get(),w,h));Nis nis(g,128);
    const auto noise=Pattern(w,h,5);
    auto inputFor=[&](UINT columns) {
        auto input=Pattern(w,h,0);
        for(UINT y=0;y<h;y++)for(UINT x=0;x<columns;x++)input[size_t(y)*w+x]=noise[size_t(y)*w+x];
        return input;
    };
    auto overflowing=[&](UINT columns) {
        Cmaa(g,p,inputFor(columns),w,h);ValidLists(g,p);return A::Words(g,p,6)[0]!=0;
    };
    UINT low=0,high=w;
    Require(!overflowing(low)&&overflowing(high),"sequence brackets real stock-capacity overflow");
    // Find adjacent safe/overflow inputs without changing any buffer capacities.
    while(high-low>1) {const UINT mid=(low+high)/2;if(overflowing(mid))high=mid;else low=mid;}
    Require(low>w/4,"unchanged comparison region remains inside noisy area");
    const auto safeInput=inputFor(low),overflowInput=inputFor(high);
    Require(Difference(safeInput,overflowInput,w,0,0,w/4,h)==0,"temporal measurement region has identical input");
    Image reference[2],aaReference[2];UINT overflowFrames=0,transitions=0;bool previous=false;
    float repeatAaMax=0,repeatNisMax=0;double repeatNisMeanMax=0;
    for(UINT frame=0;frame<24;frame++) {
        // Stable safe, threshold alternation, stable overflow. This is a stress
        // sequence, not an estimate of the frequency in gameplay or MJPEG.
        const bool expected=frame<8?false:frame<16?(frame%2!=0):true;
        const auto& input=expected?overflowInput:safeInput;
        const auto aa=Cmaa(g,p,input,w,h);const auto control=A::Words(g,p,6);ValidLists(g,p);
        Require((control[0]!=0)==expected,"threshold classification stable over sequence");
        if(expected)Require(Difference(input,aa,w,0,0,w,h)<.001f,"sequence overflow preserves source");
        const auto scaled=nis.Run(aa,w,h,ow,oh);
        auto& ref=reference[expected?1:0];
        if(ref.empty()) {ref=scaled;aaReference[expected?1:0]=aa;}
        else {
            const float aaDelta=Difference(aaReference[expected?1:0],aa,w,0,0,w,h);
            // CMAA blend order has half-float rounding variation. NIS has
            // nonlinear edge decisions: measure its amplification, rather than
            // asserting an unsupported scaled-pixel tolerance on noisy input.
            Require(aaDelta<=(expected?0.f:1.f/255),"same-state AA repeat rounding bound");
            Require(Difference(scaled,nis.Run(aa,w,h,ow,oh),ow,0,0,ow,oh)==0,"identical AA input gives identical NIS output");
            repeatAaMax=std::max(repeatAaMax,aaDelta);
            repeatNisMax=std::max(repeatNisMax,Difference(ref,scaled,ow,0,0,ow,oh));
            double mean=0;for(size_t i=0;i<scaled.size();i++)mean+=std::abs(scaled[i].r-ref[i].r);
            repeatNisMeanMax=std::max(repeatNisMeanMax,mean/scaled.size());
        }
        overflowFrames+=expected?1:0;if(frame&&expected!=previous)transitions++;previous=expected;
    }
    Require(overflowFrames==12&&transitions==7,"sequence observes both states and recovery");
    double sum=0;UINT changed=0;const UINT columns=ow/4;
    const float maximum=Difference(reference[0],reference[1],ow,0,0,columns,oh);
    for(UINT y=0;y<oh;y++)for(UINT x=0;x<columns;x++) {
        const float delta=std::abs(reference[0][size_t(y)*ow+x].r-reference[1][size_t(y)*ow+x].r);
        sum+=delta;changed+=delta>1.f/255?1:0;
    }
    std::printf("SEQUENCE: stock_capacity=%u noisy_columns=%u/%u frames=24 overflow=%u transitions=%u unchanged_region_max=%.6f mean=%.6f changed_gt_1_255=%.2f%%\n",
        w*h,low,high,overflowFrames,transitions,maximum,sum/(columns*oh),100.*changed/(columns*oh));
    std::printf("REPEAT: AA_max=%.6f NIS_max=%.6f NIS_mean_max=%.8f identical_NIS_input_delta=0\n",
        repeatAaMax,repeatNisMax,repeatNisMeanMax);
    std::puts("PASS: stock-capacity threshold sequence, unchanged input region measured after CMAA2 -> NIS 0.15 (not a gameplay frequency sample)");
}
int main(int argc,char** argv) try {
    bool warp=false,cmaaOnly=false;
    for(int i=1;i<argc;i++) {
        if(std::string(argv[i])=="--warp")warp=true;
        else if(std::string(argv[i])=="--cmaa-only")cmaaOnly=true;
        else Require(false,"unknown option");
    }
    ListValidatorTests();Gpu g(warp);if(!cmaaOnly)Boundaries(g);Overflow(g);FlatTransitions(g);OverflowSequence(g);
    std::printf("PASS: upscaling safety (%s)\n",warp?"WARP":"hardware");return 0;
} catch(const std::exception& e) {std::fprintf(stderr,"FAIL: %s\n",e.what());return 1;}
