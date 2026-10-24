#include "SpatialUpscaler.h"
#include <d3dcompiler.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <filesystem>
#include <fstream>
#include <sstream>
#include "../../third_party/upscaling/nis/NIS_Config.h"
#define A_CPU 1
#include "../../third_party/upscaling/fsr/ffx_a.h"
#include "../../third_party/upscaling/fsr/ffx_fsr1.h"
#include "../../third_party/upscaling/cas/ffx_cas.h"
#include "../../third_party/upscaling/smaa/AreaTex.h"
#include "../../third_party/upscaling/smaa/SearchTex.h"
#include "../../third_party/upscaling/cmaa2/CMAA2.hlsl"

namespace llcv::spatial {
using Microsoft::WRL::ComPtr;
LiveTrace::LiveTrace() {
    wchar_t path[32768]{};const auto n=GetEnvironmentVariableW(L"LLCV_SPATIAL_TRACE",path,32768);
    if(n && n<32768) {path_=path;frames_.reserve(8192);}
}
LiveTrace::~LiveTrace() {
    if(path_.empty())return;
    FILE* f=nullptr;
    if(_wfopen_s(&f,path_.c_str(),L"wb")==0 && f) {
        LARGE_INTEGER frequency{};QueryPerformanceFrequency(&frequency);
        std::fprintf(f,"arrival_us,present_qpc,returned_us,qpc_frequency\n");
        for(const auto& v:frames_)std::fprintf(f,"%lld,%lld,%lld,%lld\n",v.arrivalUs,v.presentQpc,v.returnedUs,frequency.QuadPart);
        std::fclose(f);
    }
    if(!input_.empty() && _wfopen_s(&f,(path_+(inputHdr_?L".p010":L".nv12")).c_str(),L"wb")==0 && f) {
        std::fwrite(input_.data(),1,input_.size(),f);std::fclose(f);
    }
    if(outputHdr_ && !output_.empty() && _wfopen_s(&f,(path_+L".rgb10").c_str(),L"wb")==0 && f) {
        std::fwrite(&outputWidth_,sizeof(outputWidth_),1,f);std::fwrite(&outputHeight_,sizeof(outputHeight_),1,f);
        std::fwrite(output_.data(),1,output_.size(),f);std::fclose(f);
    }
    if(!outputHdr_ && !output_.empty() && _wfopen_s(&f,(path_+L".bmp").c_str(),L"wb")==0 && f) {
        BITMAPFILEHEADER file{};BITMAPINFOHEADER info{};
        file.bfType=0x4d42;file.bfOffBits=sizeof(file)+sizeof(info);file.bfSize=file.bfOffBits+DWORD(output_.size());
        info.biSize=sizeof(info);info.biWidth=LONG(outputWidth_);info.biHeight=-LONG(outputHeight_);
        info.biPlanes=1;info.biBitCount=32;info.biCompression=BI_RGB;
        std::fwrite(&file,1,sizeof(file),f);std::fwrite(&info,1,sizeof(info),f);
        std::fwrite(output_.data(),1,output_.size(),f);std::fclose(f);
    }
}
void LiveTrace::Add(int64_t a,int64_t p,int64_t r) {
    if(!path_.empty() && frames_.size()<8192)frames_.push_back({a,p,r});
}
void LiveTrace::Snapshot(const unsigned char* p,size_t bytes,bool hdr) {
    // Fixed live experiment is tightly packed 1080p NV12/P010. Snapshot during warmup,
    // before the 3-second measurement cutoff; never copy every frame.
    const size_t expected=1920u*1080u*3u/(hdr?1u:2u);
    if(!path_.empty() && input_.empty() && frames_.size()==60 && p && bytes==expected) {
        inputHdr_=hdr;input_.assign(p,p+bytes);
    }
}
void LiveTrace::SnapshotOutput(ID3D11Device* d,ID3D11DeviceContext* c,ID3D11Texture2D* source) {
    if(input_.empty() || !output_.empty() || frames_.size()!=60)return;
    D3D11_TEXTURE2D_DESC desc{};source->GetDesc(&desc);
    if(desc.Format!=DXGI_FORMAT_B8G8R8A8_UNORM && desc.Format!=DXGI_FORMAT_R10G10B10A2_UNORM)return;
    outputHdr_=desc.Format==DXGI_FORMAT_R10G10B10A2_UNORM;
    desc.BindFlags=desc.MiscFlags=0;desc.Usage=D3D11_USAGE_STAGING;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;if(FAILED(d->CreateTexture2D(&desc,nullptr,&staging)))return;
    c->CopyResource(staging.Get(),source);D3D11_MAPPED_SUBRESOURCE mapped{};
    if(FAILED(c->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped)))return;
    outputWidth_=desc.Width;outputHeight_=desc.Height;output_.resize(size_t(desc.Width)*desc.Height*4);
    for(UINT y=0;y<desc.Height;y++)std::memcpy(output_.data()+size_t(y)*desc.Width*4,
        static_cast<const unsigned char*>(mapped.pData)+size_t(y)*mapped.RowPitch,size_t(desc.Width)*4);
    c->Unmap(staging.Get(),0);
}
const char* Name(Mode m) {
    switch(m) { case Mode::Bicubic:return "bicubic"; case Mode::Cas:return "cas";
    case Mode::Fsr1:return "fsr1"; case Mode::Nis:return "nis"; case Mode::Sgsr:return "sgsr";
    case Mode::Ravu:return "ravu"; case Mode::Fsrcnnx:return "fsrcnnx";
    case Mode::Jinc:return "jinc"; case Mode::Lanczos:return "lanczos"; default:return "baseline"; }
}
Mode EnvironmentMode() {
    wchar_t value[32]{};
    const DWORD n=GetEnvironmentVariableW(L"LLCV_SPATIAL_UPSCALER",value,32);
    if(!n || n>=32) return Mode::Disabled;
    if(_wcsicmp(value,L"nis15")==0 || _wcsicmp(value,L"cmaa2-nis15")==0)return Mode::Nis;
    for(auto m : {Mode::Bicubic,Mode::Cas,Mode::Fsr1,Mode::Nis,Mode::Sgsr,Mode::Ravu,Mode::Fsrcnnx,Mode::Jinc,Mode::Lanczos}) {
        std::string name=Name(m); std::wstring wide(name.begin(),name.end());
        if(_wcsicmp(value,wide.c_str())==0) return m;
    }
    return Mode::Disabled;
}
AaMode EnvironmentAaMode() {
    wchar_t value[32]{};GetEnvironmentVariableW(L"LLCV_SPATIAL_UPSCALER",value,32);
    return _wcsicmp(value,L"cmaa2-nis15")==0?AaMode::Cmaa2BeforeNis:AaMode::Disabled;
}
float EnvironmentNisSharpness() {
    wchar_t value[32]{};GetEnvironmentVariableW(L"LLCV_SPATIAL_UPSCALER",value,32);
    return (_wcsicmp(value,L"nis15")==0 || _wcsicmp(value,L"cmaa2-nis15")==0)?.15f:.5f;
}
bool Eligible(Mode m,bool hdr,unsigned w,unsigned h,const RECT& r) {
    const auto ow=r.right-r.left,oh=r.bottom-r.top;
    // Same policy for every comparison: 1x-2x per axis, SDR, enlargement only.
    return m!=Mode::Disabled && !hdr && w>0 && h>0 && r.left>=0 && r.top>=0 &&
        ow>=static_cast<LONG>(w) && oh>=static_cast<LONG>(h) &&
        ow<=static_cast<LONG>(w*2) && oh<=static_cast<LONG>(h*2) &&
        (ow>static_cast<LONG>(w) || oh>static_cast<LONG>(h));
}
void Pipeline::Reset() { *this=Pipeline{}; }
static HRESULT Compile(const wchar_t* file,const char* entry,const char* profile,
                       const D3D_SHADER_MACRO* macros,ID3DBlob** result) {
    const std::wstring path=std::wstring(LLCV_SPATIAL_SOURCE_ROOT)+L"/"+file;
    ComPtr<ID3DBlob> error;
    const HRESULT hr=D3DCompileFromFile(path.c_str(),macros,D3D_COMPILE_STANDARD_FILE_INCLUDE,
        entry,profile,D3DCOMPILE_OPTIMIZATION_LEVEL3|D3DCOMPILE_ENABLE_STRICTNESS,0,result,&error);
    if(FAILED(hr)) std::fprintf(stderr,"spatial shader: %s\n",error ?
        static_cast<const char*>(error->GetBufferPointer()) : "file/compile error");
    return hr;
}
HRESULT SmaaPipeline::Configure(ID3D11Device* d,UINT w,UINT h,bool ultraColor) {
    *this=SmaaPipeline{};
    if(!d || !w || !h)return E_INVALIDARG;
    width_=w;height_=h;
    for(unsigned i=0;i<3;i++) {
        D3D11_TEXTURE2D_DESC desc{};desc.Width=w;desc.Height=h;
        desc.MipLevels=desc.ArraySize=desc.SampleDesc.Count=1;
        desc.Format=i==2?DXGI_FORMAT_R16G16B16A16_FLOAT:DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
        ComPtr<ID3D11Texture2D> tex;HRESULT hr=d->CreateTexture2D(&desc,nullptr,&tex);if(FAILED(hr))return hr;
        hr=d->CreateRenderTargetView(tex.Get(),nullptr,&targets_[i]);if(FAILED(hr))return hr;
        hr=d->CreateShaderResourceView(tex.Get(),nullptr,&views_[i]);if(FAILED(hr))return hr;
        const auto pass=std::to_string(i);D3D_SHADER_MACRO macros[]={{"SMAA_PASS",pass.c_str()},{"SMAA_ULTRA_COLOR",ultraColor?"1":"0"},{nullptr,nullptr}};
        ComPtr<ID3DBlob> code;
        hr=Compile(L"src/video/shaders/Smaa.hlsl","Vertex","vs_5_0",macros,&code);if(FAILED(hr))return hr;
        hr=d->CreateVertexShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&vertex_[i]);if(FAILED(hr))return hr;
        hr=Compile(L"src/video/shaders/Smaa.hlsl","Pixel","ps_5_0",macros,code.ReleaseAndGetAddressOf());if(FAILED(hr))return hr;
        hr=d->CreatePixelShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&pixel_[i]);if(FAILED(hr))return hr;
    }
    auto lut=[&](UINT lw,UINT lh,DXGI_FORMAT format,const void* data,UINT pitch,ID3D11ShaderResourceView** view) {
        D3D11_TEXTURE2D_DESC desc{};desc.Width=lw;desc.Height=lh;
        desc.MipLevels=desc.ArraySize=desc.SampleDesc.Count=1;desc.Format=format;
        desc.Usage=D3D11_USAGE_IMMUTABLE;desc.BindFlags=D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA initial{data,pitch,0};ComPtr<ID3D11Texture2D> tex;
        HRESULT hr=d->CreateTexture2D(&desc,&initial,&tex);
        return FAILED(hr)?hr:d->CreateShaderResourceView(tex.Get(),nullptr,view);
    };
    static_assert(sizeof(areaTexBytes)==AREATEX_SIZE && sizeof(searchTexBytes)==SEARCHTEX_SIZE);
    HRESULT hr=lut(AREATEX_WIDTH,AREATEX_HEIGHT,DXGI_FORMAT_R8G8_UNORM,areaTexBytes,AREATEX_PITCH,&area_);if(FAILED(hr))return hr;
    hr=lut(SEARCHTEX_WIDTH,SEARCHTEX_HEIGHT,DXGI_FORMAT_R8_UNORM,searchTexBytes,SEARCHTEX_PITCH,&search_);if(FAILED(hr))return hr;
    const float metrics[]={1.0f/w,1.0f/h,float(w),float(h)};
    D3D11_BUFFER_DESC cb{};cb.ByteWidth=sizeof(metrics);cb.Usage=D3D11_USAGE_IMMUTABLE;cb.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SUBRESOURCE_DATA initial{metrics,0,0};hr=d->CreateBuffer(&cb,&initial,&metrics_);if(FAILED(hr))return hr;
    for(unsigned i=0;i<2;i++) {
        D3D11_SAMPLER_DESC sam{};sam.Filter=i?D3D11_FILTER_MIN_MAG_MIP_POINT:D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
        sam.AddressU=sam.AddressV=sam.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;sam.MaxLOD=D3D11_FLOAT32_MAX;
        hr=d->CreateSamplerState(&sam,&samplers_[i]);if(FAILED(hr))return hr;
    }
    return S_OK;
}
ID3D11ShaderResourceView* SmaaPipeline::Draw(ID3D11DeviceContext* c,ID3D11ShaderResourceView* input) {
    // Both edge/weight targets are cleared every frame: edge detection discards.
    const float zero[4]{};c->ClearRenderTargetView(targets_[0].Get(),zero);c->ClearRenderTargetView(targets_[1].Get(),zero);
    D3D11_VIEWPORT viewport{0,0,float(width_),float(height_),0,1};c->RSSetViewports(1,&viewport);
    c->VSSetConstantBuffers(0,1,metrics_.GetAddressOf());c->PSSetConstantBuffers(0,1,metrics_.GetAddressOf());
    ID3D11SamplerState* samplers[]={samplers_[0].Get(),samplers_[1].Get()};c->PSSetSamplers(0,2,samplers);
    ID3D11ShaderResourceView* inputs[3][3]={{input,nullptr,nullptr},{views_[0].Get(),area_.Get(),search_.Get()},{input,views_[1].Get(),nullptr}};
    for(unsigned i=0;i<3;i++) {
        c->VSSetShader(vertex_[i].Get(),nullptr,0);c->PSSetShader(pixel_[i].Get(),nullptr,0);
        c->OMSetRenderTargets(1,targets_[i].GetAddressOf(),nullptr);c->PSSetShaderResources(0,3,inputs[i]);c->Draw(3,0);
        ID3D11ShaderResourceView* empty[3]{};c->PSSetShaderResources(0,3,empty);c->OMSetRenderTargets(0,nullptr,nullptr);
    }
    return views_[2].Get();
}

// Only the pinned legacy DDS format used by the RAVU weights is accepted.
static HRESULT LoadRavuLut(ID3D11Device* d,const wchar_t* name,ID3D11ShaderResourceView** view) {
    std::ifstream in(std::filesystem::path(LLCV_SPATIAL_SOURCE_ROOT)/L"third_party/upscaling/magpie/RAVU"/name,std::ios::binary);
    uint32_t header[32]{};
    if(!in.read(reinterpret_cast<char*>(header),sizeof(header)) || header[0]!=0x20534444 ||
       header[1]!=124 || header[3]!=2592 || header[4]!=18 || header[19]!=32 ||
       header[21]!=113 || header[7]!=1) return E_INVALIDARG;
    std::vector<char> data(18*2592*8);
    if(!in.read(data.data(),std::streamsize(data.size())) || in.peek()!=std::char_traits<char>::eof())return E_INVALIDARG;
    D3D11_TEXTURE2D_DESC desc{};desc.Width=18;desc.Height=2592;
    desc.MipLevels=desc.ArraySize=desc.SampleDesc.Count=1;desc.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;
    desc.Usage=D3D11_USAGE_IMMUTABLE;desc.BindFlags=D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA initial{data.data(),18*8,0};ComPtr<ID3D11Texture2D> tex;
    HRESULT hr=d->CreateTexture2D(&desc,&initial,&tex);
    return FAILED(hr)?hr:d->CreateShaderResourceView(tex.Get(),nullptr,view);
}
// Adapt Magpie's explicit pass declarations, retaining all original model math
// and weights. Compilation and file access happen only on Configure, never Draw.
static HRESULT CompileLearned(Mode mode,unsigned pass,ID3DBlob** result) {
    const auto root=std::filesystem::path(LLCV_SPATIAL_SOURCE_ROOT)/L"third_party/upscaling/magpie";
    const auto file=root/(mode==Mode::Ravu?L"RAVU/RAVU_Zoom_AR_R2_RGB.hlsl":L"FSRCNNX/FSRCNNX.hlsl");
    std::ifstream input(file);std::stringstream text;text<<input.rdbuf();
    if(!input)return E_FAIL;
    const auto source=text.str(),marker=std::string("//!PASS ")+std::to_string(pass);
    const auto begin=source.find(marker);
    if(begin==std::string::npos)return E_INVALIDARG;
    const auto end=source.find("//!PASS ",begin+marker.size());
    const auto body=source.substr(begin,end==std::string::npos?end:end-begin);
    std::string prefix=R"(
cbuffer LearnedSettings : register(b0) { float4 sizes; };
uint2 GetInputSize() { return uint2(sizes.xy); }
float2 GetInputPt() { return 1.0/sizes.xy; }
uint2 GetOutputSize() { return uint2(sizes.zw); }
float2 GetOutputPt() { return 1.0/sizes.zw; }
#define MF float
#define MF2 float2
#define MF3 float3
#define MF4 float4
#define MF2x3 float2x3
#define MF3x3 float3x3
#define MF4x4 float4x4
// Match the runtime helpers in pinned Magpie EffectCompiler.cpp, not its
// IDE-only StubDefs. The ordering and mad chain matter for GPU efficiency.
float4 MulAdd(float4 x,float4x4 y,float4 a) {
    a=mad(x.x,y[0],a);a=mad(x.y,y[1],a);
    a=mad(x.z,y[2],a);return mad(x.w,y[3],a);
}
uint2 Rmp8x8(uint a) { return uint2((a>>1)&7,((a>>3)&6)|(a&1)); }
)";
    unsigned bw=8,bh=8,tx=64,ty=1;
    if(mode==Mode::Ravu) {
        bw=32;bh=8;tx=32;ty=8;
        prefix+=R"(
Texture2D<float4> INPUT : register(t0);
Texture2D<float4> ravu_zoom_lut2 : register(t1);
Texture2D<float4> ravu_zoom_lut2_ar : register(t2);
RWTexture2D<float4> OUTPUT : register(u0);
SamplerState sam_INPUT : register(s0);
SamplerState sam_ravu_zoom_lut2 : register(s1);
#define sam_ravu_zoom_lut2_ar sam_ravu_zoom_lut2
#include "prescalers.hlsli"
#define LAST_PASS 1
)";
    } else {
        static const char* inputs[]={"INPUT","featureMap1,featureMap2","tex1,tex2","tex3,tex4",
            "tex1,tex2,featureMap1,featureMap2","tex3,tex4,INPUT"};
        static const char* outputs[]={"featureMap1,featureMap2","tex1,tex2","tex3,tex4","tex1,tex2","tex3,tex4","OUTPUT"};
        if(pass<1 || pass>6)return E_INVALIDARG;
        auto bindings=[&](const char* names,bool output) {
            std::stringstream list(names);std::string name;unsigned index=0;
            while(std::getline(list,name,','))prefix+=(output?"RWTexture2D<float4> ":"Texture2D<float4> ")+name+
                " : register("+std::string(output?"u":"t")+std::to_string(index++)+");\n";
        };
        bindings(inputs[pass-1],false);bindings(outputs[pass-1],true);
        prefix+="SamplerState sam : register(s0);\nSamplerState sam1 : register(s1);\n";
        if(pass==1){bw=32;bh=24;tx=128;}
        if(pass==6){bw=16;bh=16;}
    }
    prefix+="#define MP_BLOCK_WIDTH "+std::to_string(bw)+"\n#define MP_BLOCK_HEIGHT "+std::to_string(bh)+
        "\n#define MP_NUM_THREADS_X "+std::to_string(tx)+"\n#define MP_NUM_THREADS_Y "+std::to_string(ty)+"\n";
    const std::string code=prefix+body+"\n[numthreads("+std::to_string(tx)+","+std::to_string(ty)+
        ",1)] void main(uint3 group : SV_GroupID,uint3 thread : SV_GroupThreadID) { Pass"+
        std::to_string(pass)+"(group.xy*uint2(MP_BLOCK_WIDTH,MP_BLOCK_HEIGHT),thread); }";
    ComPtr<ID3DBlob> error;
    HRESULT hr=D3DCompile(code.data(),code.size(),file.string().c_str(),nullptr,D3D_COMPILE_STANDARD_FILE_INCLUDE,
        "main","cs_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3|D3DCOMPILE_ENABLE_STRICTNESS,0,result,&error);
    if(FAILED(hr))std::fprintf(stderr,"learned shader: %s\n",error?static_cast<const char*>(error->GetBufferPointer()):"compile error");
    return hr;
}
HRESULT Pipeline::Configure(ID3D11Device* d,ID3D11VideoDevice* vd,ID3D11VideoProcessorEnumerator* e,
                            Mode m,bool hdr,UINT w,UINT h,const RECT& r,UINT vendor,float nisSharpness,AaMode smaa) {
    Reset();
    if(smaa!=AaMode::Disabled && (m!=Mode::Nis || (!AaBeforeNis(smaa) && smaa!=AaMode::SmaaAfterNis && smaa!=AaMode::SmaaUltraAfterNis && smaa!=AaMode::Cmaa2AfterNis)))return E_INVALIDARG;
    if(m==Mode::Nis && (!std::isfinite(nisSharpness) || nisSharpness<0 || nisSharpness>1)) return E_INVALIDARG;
    if(!Eligible(m,hdr,w,h,r)) return S_FALSE;
    if(!d || !vd || !e) return E_POINTER;
    width_=w; height_=h; destination_=r; scaledWidth_=r.right-r.left; scaledHeight_=r.bottom-r.top;
    auto texture=[&](UINT tw,UINT th,DXGI_FORMAT format,UINT flags,ID3D11Texture2D** out) {
        D3D11_TEXTURE2D_DESC desc{};desc.Width=tw;desc.Height=th;desc.Format=format;
        desc.MipLevels=desc.ArraySize=desc.SampleDesc.Count=1;desc.BindFlags=flags;
        return d->CreateTexture2D(&desc,nullptr,out);
    };
    HRESULT hr=texture(w,h,DXGI_FORMAT_B8G8R8A8_UNORM,D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE,&input_);
    if(FAILED(hr)) return hr;
    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ov{};ov.ViewDimension=D3D11_VPOV_DIMENSION_TEXTURE2D;
    hr=vd->CreateVideoProcessorOutputView(input_.Get(),e,&ov,&inputOutput_);
    if(FAILED(hr)) return hr;
    hr=d->CreateShaderResourceView(input_.Get(),nullptr,&inputView_);if(FAILED(hr)) return hr;
    if(m==Mode::Fsr1 || m==Mode::Nis || m==Mode::Ravu || m==Mode::Fsrcnnx) {
        hr=texture(m==Mode::Fsrcnnx?w*2:scaledWidth_,m==Mode::Fsrcnnx?h*2:scaledHeight_,DXGI_FORMAT_R16G16B16A16_FLOAT,
            D3D11_BIND_SHADER_RESOURCE|(m!=Mode::Fsr1?D3D11_BIND_UNORDERED_ACCESS:D3D11_BIND_RENDER_TARGET),&intermediate_);
        if(FAILED(hr)) return hr;
        hr=d->CreateShaderResourceView(intermediate_.Get(),nullptr,&intermediateView_);if(FAILED(hr)) return hr;
        if(m!=Mode::Fsr1) hr=d->CreateUnorderedAccessView(intermediate_.Get(),nullptr,&intermediateUav_);
        else hr=d->CreateRenderTargetView(intermediate_.Get(),nullptr,&intermediateTarget_);
        if(FAILED(hr)) return hr;
    }
    ComPtr<ID3DBlob> code;
    const wchar_t* shader=L"src/video/shaders/Spatial.hlsl";
    hr=Compile(shader,"Vertex","vs_5_0",nullptr,&code);if(FAILED(hr)) return hr;
    hr=d->CreateVertexShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&vertex_);if(FAILED(hr)) return hr;
    D3D_SHADER_MACRO defines[]={{m==Mode::Cas?"METHOD_CAS":m==Mode::Fsr1?"METHOD_FSR":m==Mode::Bicubic?"METHOD_BICUBIC":m==Mode::Sgsr?"METHOD_SGSR":
        m==Mode::Jinc?"METHOD_JINC":(m==Mode::Lanczos || m==Mode::Fsrcnnx)?"METHOD_LANCZOS":"METHOD_COPY","1"},{nullptr,nullptr}};
    hr=Compile(shader,"Pixel","ps_5_0",defines,code.ReleaseAndGetAddressOf());if(FAILED(hr)) return hr;
    hr=d->CreatePixelShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&pixel_);if(FAILED(hr)) return hr;
    if(m==Mode::Fsr1) {
        defines[0].Name="METHOD_RCAS";
        hr=Compile(shader,"Pixel","ps_5_0",defines,code.ReleaseAndGetAddressOf());if(FAILED(hr)) return hr;
        hr=d->CreatePixelShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&second_);if(FAILED(hr)) return hr;
    }
    struct alignas(16) Constants { uint32_t con[4][4]{},rcas[4]{};float dimensions[4]{},placement[4]{}; } c;
    if(m==Mode::Cas) CasSetup(c.con[0],c.con[1],.5f,float(w),float(h),float(scaledWidth_),float(scaledHeight_));
    if(m==Mode::Fsr1) {
        FsrEasuCon(c.con[0],c.con[1],c.con[2],c.con[3],float(w),float(h),float(w),float(h),float(scaledWidth_),float(scaledHeight_));
        FsrRcasCon(c.rcas,1.0f); // Moderate sharpening; 1 stop below maximum.
    }
    c.dimensions[0]=float(w);c.dimensions[1]=float(h);c.dimensions[2]=float(scaledWidth_);c.dimensions[3]=float(scaledHeight_);
    if(m==Mode::Fsrcnnx){c.dimensions[0]*=2;c.dimensions[1]*=2;}
    c.placement[0]=float(r.left);c.placement[1]=float(r.top);
    auto buffer=[&](const void* data,UINT size,ID3D11Buffer** out) {
        D3D11_BUFFER_DESC b{};b.ByteWidth=size;b.Usage=D3D11_USAGE_IMMUTABLE;b.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
        D3D11_SUBRESOURCE_DATA initial{data,0,0};return d->CreateBuffer(&b,&initial,out);
    };
    hr=buffer(&c,sizeof(c),&constants_);if(FAILED(hr)) return hr;
    D3D11_SAMPLER_DESC sam{};sam.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sam.AddressU=sam.AddressV=sam.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;sam.MaxLOD=D3D11_FLOAT32_MAX;
    hr=d->CreateSamplerState(&sam,&sampler_);if(FAILED(hr)) return hr;
    if(m==Mode::Ravu || m==Mode::Fsrcnnx) {
        sam.Filter=D3D11_FILTER_MIN_MAG_MIP_POINT;
        hr=d->CreateSamplerState(&sam,&pointSampler_);if(FAILED(hr))return hr;
        const float sizes[4]={float(w),float(h),float(m==Mode::Fsrcnnx?w*2:scaledWidth_),float(m==Mode::Fsrcnnx?h*2:scaledHeight_)};
        hr=buffer(sizes,sizeof(sizes),&learnedConstants_);if(FAILED(hr))return hr;
        if(m==Mode::Ravu) {
            hr=CompileLearned(m,1,code.ReleaseAndGetAddressOf());if(FAILED(hr))return hr;
            hr=d->CreateComputeShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&compute_);if(FAILED(hr))return hr;
            hr=LoadRavuLut(d,L"ravu_zoom_lut2_f16.dds",&coefficients_[0]);if(FAILED(hr))return hr;
            hr=LoadRavuLut(d,L"ravu_zoom_lut2_ar_f16.dds",&coefficients_[1]);if(FAILED(hr))return hr;
        } else for(unsigned i=0;i<6;i++) {
            hr=CompileLearned(m,i+1,code.ReleaseAndGetAddressOf());if(FAILED(hr))return hr;
            hr=d->CreateComputeShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&network_[i]);if(FAILED(hr))return hr;
            ComPtr<ID3D11Texture2D> feature;
            hr=texture(w,h,DXGI_FORMAT_R16G16B16A16_FLOAT,D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS,&feature);if(FAILED(hr))return hr;
            hr=d->CreateShaderResourceView(feature.Get(),nullptr,&featureViews_[i]);if(FAILED(hr))return hr;
            hr=d->CreateUnorderedAccessView(feature.Get(),nullptr,&featureUavs_[i]);if(FAILED(hr))return hr;
        }
    }
    D3D11_RASTERIZER_DESC raster{};raster.FillMode=D3D11_FILL_SOLID;raster.CullMode=D3D11_CULL_NONE;raster.DepthClipEnable=TRUE;
    hr=d->CreateRasterizerState(&raster,&rasterizer_);if(FAILED(hr)) return hr;
    if(m==Mode::Nis) {
        NISOptimizer optimizer(true,vendor==0x10de?NISGPUArchitecture::NVIDIA_Generic:
            vendor==0x8086?NISGPUArchitecture::Intel_Generic:NISGPUArchitecture::AMD_Generic);
        nisBlockWidth_=optimizer.GetOptimalBlockWidth();nisBlockHeight_=optimizer.GetOptimalBlockHeight();
        const std::string bw=std::to_string(nisBlockWidth_),bh=std::to_string(nisBlockHeight_),tg=std::to_string(optimizer.GetOptimalThreadGroupSize());
        D3D_SHADER_MACRO nis[]={{"NIS_SCALER","1"},{"NIS_HDR_MODE","0"},{"NIS_NV12_SUPPORT","0"},
            {"NIS_VIEWPORT_SUPPORT","1"},{"NIS_CLAMP_OUTPUT","1"},{"NIS_BLOCK_WIDTH",bw.c_str()},
            {"NIS_BLOCK_HEIGHT",bh.c_str()},{"NIS_THREAD_GROUP_SIZE",tg.c_str()},{nullptr,nullptr}};
        hr=Compile(L"third_party/upscaling/nis/NIS_Main.hlsl","main","cs_5_0",nis,code.ReleaseAndGetAddressOf());if(FAILED(hr)) return hr;
        hr=d->CreateComputeShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&compute_);if(FAILED(hr)) return hr;
        NISConfig config{};
        if(!NVScalerUpdateConfig(config,nisSharpness,0,0,w,h,w,h,0,0,scaledWidth_,scaledHeight_,scaledWidth_,scaledHeight_)) return E_INVALIDARG;
        hr=buffer(&config,sizeof(config),&nisConstants_);if(FAILED(hr)) return hr;
        for(int i=0;i<2;i++) {
            D3D11_TEXTURE2D_DESC desc{};desc.Width=2;desc.Height=64;desc.MipLevels=desc.ArraySize=desc.SampleDesc.Count=1;
            desc.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;desc.BindFlags=D3D11_BIND_SHADER_RESOURCE;desc.Usage=D3D11_USAGE_IMMUTABLE;
            D3D11_SUBRESOURCE_DATA initial{i==0?static_cast<const void*>(coef_scale):static_cast<const void*>(coef_usm),8*sizeof(float),0};
            ComPtr<ID3D11Texture2D> tex;hr=d->CreateTexture2D(&desc,&initial,&tex);if(FAILED(hr)) return hr;
            hr=d->CreateShaderResourceView(tex.Get(),nullptr,&coefficients_[i]);if(FAILED(hr)) return hr;
        }
    }
    if(smaa!=AaMode::Disabled) {
        const UINT aaWidth=AaBeforeNis(smaa)?w:scaledWidth_,aaHeight=AaBeforeNis(smaa)?h:scaledHeight_;
        hr=AaIsCmaa2(smaa)?cmaa_.Configure(d,aaWidth,aaHeight):
            smaa_.Configure(d,aaWidth,aaHeight,smaa==AaMode::SmaaUltraAfterNis || smaa==AaMode::SmaaUltraBeforeNis);
        if(FAILED(hr))return hr;
    }
    mode_=m;aaMode_=smaa;return S_OK;
}
HRESULT Pipeline::Draw(ID3D11DeviceContext* c,ID3D11VideoContext* vc,ID3D11VideoProcessor* vp,
    D3D11_VIDEO_PROCESSOR_STREAM& stream,ID3D11RenderTargetView* target,UINT ow,UINT oh) {
    if(!Enabled()) return E_UNEXPECTED;
    const RECT native{0,0,LONG(width_),LONG(height_)},output{0,0,LONG(ow),LONG(oh)};
    // Keep the production YUV conversion/range policy. No CPU readback or frame queue.
    c->OMSetRenderTargets(0,nullptr,nullptr);
    vc->VideoProcessorSetStreamDestRect(vp,0,TRUE,&native);
    vc->VideoProcessorSetOutputTargetRect(vp,TRUE,&native);
    const HRESULT hr=vc->VideoProcessorBlt(vp,inputOutput_.Get(),0,1,&stream);
    vc->VideoProcessorSetStreamDestRect(vp,0,TRUE,&destination_);
    vc->VideoProcessorSetOutputTargetRect(vp,TRUE,&output);
    if(FAILED(hr)) return hr;
    c->OMSetBlendState(nullptr,nullptr,0xffffffff);c->OMSetDepthStencilState(nullptr,0);
    c->RSSetState(rasterizer_.Get());c->IASetInputLayout(nullptr);
    c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    c->VSSetShader(vertex_.Get(),nullptr,0);c->GSSetShader(nullptr,nullptr,0);
    c->PSSetConstantBuffers(0,1,constants_.GetAddressOf());c->PSSetSamplers(0,1,sampler_.GetAddressOf());
    auto draw=[&](ID3D11RenderTargetView* rt,ID3D11ShaderResourceView* srv,ID3D11PixelShader* ps,bool final) {
        D3D11_VIEWPORT viewport{final?float(destination_.left):0,final?float(destination_.top):0,float(scaledWidth_),float(scaledHeight_),0,1};
        c->RSSetViewports(1,&viewport);c->OMSetRenderTargets(1,&rt,nullptr);
        c->PSSetShader(ps,nullptr,0);c->PSSetShaderResources(0,1,&srv);c->Draw(3,0);
        ID3D11ShaderResourceView* empty=nullptr;c->PSSetShaderResources(0,1,&empty);c->OMSetRenderTargets(0,nullptr,nullptr);
    };
    if(mode_==Mode::Ravu || mode_==Mode::Fsrcnnx) {
        ID3D11SamplerState* samplers[]={pointSampler_.Get(),sampler_.Get()};
        c->CSSetSamplers(0,2,samplers);c->CSSetConstantBuffers(0,1,learnedConstants_.GetAddressOf());
        auto dispatch=[&](ID3D11ComputeShader* shader,ID3D11ShaderResourceView** inputs,UINT n,
            ID3D11UnorderedAccessView** outputs,UINT count,UINT gx,UINT gy) {
            c->CSSetShader(shader,nullptr,0);c->CSSetShaderResources(0,n,inputs);c->CSSetUnorderedAccessViews(0,count,outputs,nullptr);
            c->Dispatch(gx,gy,1);
            ID3D11ShaderResourceView* empty[4]{};ID3D11UnorderedAccessView* uav[2]{};
            c->CSSetShaderResources(0,4,empty);c->CSSetUnorderedAccessViews(0,2,uav,nullptr);
        };
        if(mode_==Mode::Ravu) {
            ID3D11ShaderResourceView* inputs[]={inputView_.Get(),coefficients_[0].Get(),coefficients_[1].Get()};
            dispatch(compute_.Get(),inputs,3,intermediateUav_.GetAddressOf(),1,(scaledWidth_+31)/32,(scaledHeight_+7)/8);
        } else {
            ID3D11ShaderResourceView* inputs[6][4]={
                {inputView_.Get()}, {featureViews_[0].Get(),featureViews_[1].Get()},
                {featureViews_[2].Get(),featureViews_[3].Get()}, {featureViews_[4].Get(),featureViews_[5].Get()},
                {featureViews_[2].Get(),featureViews_[3].Get(),featureViews_[0].Get(),featureViews_[1].Get()},
                {featureViews_[4].Get(),featureViews_[5].Get(),inputView_.Get()}};
            ID3D11UnorderedAccessView* outputs[6][2]={
                {featureUavs_[0].Get(),featureUavs_[1].Get()}, {featureUavs_[2].Get(),featureUavs_[3].Get()},
                {featureUavs_[4].Get(),featureUavs_[5].Get()}, {featureUavs_[2].Get(),featureUavs_[3].Get()},
                {featureUavs_[4].Get(),featureUavs_[5].Get()}, {intermediateUav_.Get()}};
            const UINT counts[]={1,2,2,2,4,3};
            for(unsigned i=0;i<6;i++)dispatch(network_[i].Get(),inputs[i],counts[i],outputs[i],i==5?1:2,
                (width_+(i==0?31:7))/(i==0?32:8),(height_+(i==0?23:7))/(i==0?24:8));
        }
        c->CSSetShader(nullptr,nullptr,0);
        draw(target,intermediateView_.Get(),pixel_.Get(),true);
    } else if(mode_==Mode::Nis) {
        auto applySmaa=[&](ID3D11ShaderResourceView* source) {
            auto result=AaIsCmaa2(aaMode_)?cmaa_.Draw(c,source):smaa_.Draw(c,source);
            c->VSSetShader(vertex_.Get(),nullptr,0);c->PSSetConstantBuffers(0,1,constants_.GetAddressOf());
            c->PSSetSamplers(0,1,sampler_.GetAddressOf());return result;
        };
        ID3D11ShaderResourceView* nisInput=inputView_.Get();
        if(AaBeforeNis(aaMode_))nisInput=applySmaa(nisInput);
        ID3D11ShaderResourceView* srvs[]={nisInput,coefficients_[0].Get(),coefficients_[1].Get()};
        c->CSSetShader(compute_.Get(),nullptr,0);c->CSSetShaderResources(0,3,srvs);
        c->CSSetSamplers(0,1,sampler_.GetAddressOf());c->CSSetConstantBuffers(0,1,nisConstants_.GetAddressOf());
        c->CSSetUnorderedAccessViews(0,1,intermediateUav_.GetAddressOf(),nullptr);
        c->Dispatch((scaledWidth_+nisBlockWidth_-1)/nisBlockWidth_,(scaledHeight_+nisBlockHeight_-1)/nisBlockHeight_,1);
        ID3D11ShaderResourceView* empty[3]{};ID3D11UnorderedAccessView* uav=nullptr;
        c->CSSetShaderResources(0,3,empty);c->CSSetUnorderedAccessViews(0,1,&uav,nullptr);c->CSSetShader(nullptr,nullptr,0);
        auto nisOutput=intermediateView_.Get();
        if(aaMode_==AaMode::SmaaAfterNis || aaMode_==AaMode::SmaaUltraAfterNis || aaMode_==AaMode::Cmaa2AfterNis)nisOutput=applySmaa(nisOutput);
        draw(target,nisOutput,pixel_.Get(),true);
    } else if(mode_==Mode::Fsr1) {
        draw(intermediateTarget_.Get(),inputView_.Get(),pixel_.Get(),false);
        draw(target,intermediateView_.Get(),second_.Get(),true);
    } else draw(target,inputView_.Get(),pixel_.Get(),true);
    c->RSSetState(nullptr);
    return S_OK;
}
}
