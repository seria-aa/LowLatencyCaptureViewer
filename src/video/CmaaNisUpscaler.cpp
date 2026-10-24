#include "CmaaNisUpscaler.h"
#include <cmath>
#include "../../third_party/upscaling/nis/NIS_Config.h"
#include "../../third_party/upscaling/cmaa2/CMAA2.hlsl"
#include "cmaaEdges.h"
#include "cmaaArgs.h"
#include "cmaaCandidates.h"
#include "cmaaApply.h"
#include "transferVertex.h"
#include "decode709.h"
#include "encode709.h"
#include "copyPixel.h"
#include "nis128.h"
#include "nis256.h"
namespace llcv::upscaling {
using Microsoft::WRL::ComPtr;
bool Eligible(bool enabled, unsigned w, unsigned h, const RECT& r) {
    if(!enabled || !w || !h || w>4096 || h>4096 || r.left<0 || r.top<0 ||
        r.right<=r.left || r.bottom<=r.top) return false;
    const auto dw=static_cast<unsigned>(r.right-r.left),dh=static_cast<unsigned>(r.bottom-r.top);
    return dw>=w && dh>=h && (dw>w || dh>h) && dw<=2*w && dh<=2*h;
}
void Pipeline::Reset() { *this=Pipeline{}; }
HRESULT CmaaPipeline::Configure(ID3D11Device* d,UINT w,UINT h) {
    *this=CmaaPipeline{};if(!d || !w || !h || w>4096 || h>4096)return E_INVALIDARG;
    width_=w;height_=h;
    auto texture=[&](UINT tw,UINT th,DXGI_FORMAT format,UINT flags,ID3D11Texture2D** tex) {
        D3D11_TEXTURE2D_DESC desc{};desc.Width=tw;desc.Height=th;desc.Format=format;
        desc.MipLevels=desc.ArraySize=desc.SampleDesc.Count=1;desc.BindFlags=flags;
        return d->CreateTexture2D(&desc,nullptr,tex);
    };
    ComPtr<ID3D11Texture2D> tex;
    HRESULT hr=texture(w,h,DXGI_FORMAT_R16G16B16A16_FLOAT,D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS,&tex);if(FAILED(hr))return hr;
    hr=d->CreateRenderTargetView(tex.Get(),nullptr,&linearTarget_);if(FAILED(hr))return hr;
    hr=d->CreateShaderResourceView(tex.Get(),nullptr,&linearView_);if(FAILED(hr))return hr;
    hr=d->CreateUnorderedAccessView(tex.Get(),nullptr,&uavs_[0]);if(FAILED(hr))return hr;
    hr=texture(w,h,DXGI_FORMAT_R16G16B16A16_FLOAT,D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE,tex.ReleaseAndGetAddressOf());if(FAILED(hr))return hr;
    hr=d->CreateRenderTargetView(tex.Get(),nullptr,&encodedTarget_);if(FAILED(hr))return hr;
    hr=d->CreateShaderResourceView(tex.Get(),nullptr,&encodedView_);if(FAILED(hr))return hr;
    hr=texture((w+1)/2,h,DXGI_FORMAT_R8_UINT,D3D11_BIND_UNORDERED_ACCESS,tex.ReleaseAndGetAddressOf());if(FAILED(hr))return hr;
    hr=d->CreateUnorderedAccessView(tex.Get(),nullptr,&uavs_[1]);if(FAILED(hr))return hr;
    hr=texture((w+1)/2,(h+1)/2,DXGI_FORMAT_R32_UINT,D3D11_BIND_UNORDERED_ACCESS,tex.ReleaseAndGetAddressOf());if(FAILED(hr))return hr;
    hr=d->CreateUnorderedAccessView(tex.Get(),nullptr,&uavs_[5]);if(FAILED(hr))return hr;
    auto buffer=[&](unsigned slot,UINT elements,UINT stride,bool raw,bool indirect) {
        D3D11_BUFFER_DESC desc{};desc.ByteWidth=elements*stride;desc.BindFlags=D3D11_BIND_UNORDERED_ACCESS;
        desc.MiscFlags=raw?D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS:D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        if(indirect)desc.MiscFlags|=D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS;
        desc.StructureByteStride=raw?0:stride;
        ComPtr<ID3D11Buffer> resource;HRESULT result=d->CreateBuffer(&desc,nullptr,&resource);if(FAILED(result))return result;
        D3D11_UNORDERED_ACCESS_VIEW_DESC view{};view.ViewDimension=D3D11_UAV_DIMENSION_BUFFER;
        view.Format=raw?DXGI_FORMAT_R32_TYPELESS:DXGI_FORMAT_UNKNOWN;view.Buffer.NumElements=elements;
        view.Buffer.Flags=raw?D3D11_BUFFER_UAV_FLAG_RAW:0;
        result=d->CreateUnorderedAccessView(resource.Get(),&view,&uavs_[slot]);
        if(indirect)indirect_=resource;return result;
    };
    // Fully sized edge lists preserve dense details without dropping work.
    hr=buffer(2,w*h,4,false,false);if(FAILED(hr))return hr;
    hr=buffer(3,((w+1)/2)*((h+1)/2),4,false,false);if(FAILED(hr))return hr;
    hr=buffer(4,w*h,8,false,false);if(FAILED(hr))return hr;
    hr=buffer(6,16,4,true,false);if(FAILED(hr))return hr;
    hr=buffer(7,4,4,true,true);if(FAILED(hr))return hr;
    struct Code { const BYTE* data; size_t size; };
    const Code codes[] = {
        {cmaaEdges, sizeof(cmaaEdges)},
        {cmaaArgs, sizeof(cmaaArgs)},
        {cmaaCandidates, sizeof(cmaaCandidates)},
        {cmaaApply, sizeof(cmaaApply)}
    };
    for (unsigned i=0;i<4;i++) {
        hr=d->CreateComputeShader(codes[i].data,codes[i].size,nullptr,&compute_[i]);if(FAILED(hr))return hr;
    }
    hr=d->CreateVertexShader(transferVertex,sizeof(transferVertex),nullptr,&vertex_);if(FAILED(hr))return hr;
    hr=d->CreatePixelShader(decode709,sizeof(decode709),nullptr,&decode_);if(FAILED(hr))return hr;
    return d->CreatePixelShader(encode709,sizeof(encode709),nullptr,&encode_);
}
ID3D11ShaderResourceView* CmaaPipeline::Draw(ID3D11DeviceContext* c,ID3D11ShaderResourceView* input) {
    D3D11_VIEWPORT vp{0,0,float(width_),float(height_),0,1};c->RSSetViewports(1,&vp);
    auto transfer=[&](ID3D11ShaderResourceView* source,ID3D11RenderTargetView* target,ID3D11PixelShader* shader) {
        c->VSSetShader(vertex_.Get(),nullptr,0);c->PSSetShader(shader,nullptr,0);
        c->OMSetRenderTargets(1,&target,nullptr);c->PSSetShaderResources(0,1,&source);c->Draw(3,0);
        ID3D11ShaderResourceView* empty=nullptr;c->PSSetShaderResources(0,1,&empty);c->OMSetRenderTargets(0,nullptr,nullptr);
    };
    transfer(input,linearTarget_.Get(),decode_.Get());
    const UINT zero[4]{};c->ClearUnorderedAccessViewUint(uavs_[6].Get(),zero);
    ID3D11UnorderedAccessView* uavs[8]{};for(unsigned i=1;i<8;i++)uavs[i]=uavs_[i].Get();
    c->CSSetUnorderedAccessViews(0,8,uavs,nullptr);c->CSSetShaderResources(0,1,linearView_.GetAddressOf());
    c->CSSetShader(compute_[0].Get(),nullptr,0);
    constexpr UINT blockX=(CMAA2_CS_INPUT_KERNEL_SIZE_X-2)*2,blockY=(CMAA2_CS_INPUT_KERNEL_SIZE_Y-2)*2;
    c->Dispatch((width_+blockX-1)/blockX,(height_+blockY-1)/blockY,1);
    c->CSSetShader(compute_[1].Get(),nullptr,0);c->Dispatch(2,1,1);
    c->CSSetShader(compute_[2].Get(),nullptr,0);c->DispatchIndirect(indirect_.Get(),0);
    c->CSSetShader(compute_[1].Get(),nullptr,0);c->Dispatch(1,2,1);
    ID3D11ShaderResourceView* empty=nullptr;c->CSSetShaderResources(0,1,&empty);
    uavs[0]=uavs_[0].Get();c->CSSetUnorderedAccessViews(0,8,uavs,nullptr);
    c->CSSetShader(compute_[3].Get(),nullptr,0);c->DispatchIndirect(indirect_.Get(),0);
    ID3D11UnorderedAccessView* clear[8]{};c->CSSetUnorderedAccessViews(0,8,clear,nullptr);c->CSSetShader(nullptr,nullptr,0);
    transfer(linearView_.Get(),encodedTarget_.Get(),encode_.Get());return encodedView_.Get();
}

HRESULT Pipeline::Configure(ID3D11Device* d,ID3D11VideoDevice* vd,ID3D11VideoProcessorEnumerator* e,
                            bool enabled,bool hdr,UINT w,UINT h,const RECT& r,UINT vendor,float nisSharpness) {
    Reset();
    // Preserve the requested SDR setting, but never allocate or run this
    // display filter for HDR. The renderer retains its original HDR path.
    if(hdr) return S_FALSE;
    if(!std::isfinite(nisSharpness) || nisSharpness<0 || nisSharpness>1) return E_INVALIDARG;
    if(!Eligible(enabled,w,h,r)) return S_FALSE;
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
    hr=texture(scaledWidth_,scaledHeight_,DXGI_FORMAT_R16G16B16A16_FLOAT,
        D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS,&intermediate_);if(FAILED(hr))return hr;
    hr=d->CreateShaderResourceView(intermediate_.Get(),nullptr,&intermediateView_);if(FAILED(hr))return hr;
    hr=d->CreateUnorderedAccessView(intermediate_.Get(),nullptr,&intermediateUav_);if(FAILED(hr))return hr;
    hr=d->CreatePixelShader(copyPixel,sizeof(copyPixel),nullptr,&pixel_);if(FAILED(hr))return hr;
    struct alignas(16) Constants { float placement[4]{}; } c;
    c.placement[0]=float(r.left);c.placement[1]=float(r.top);
    auto buffer=[&](const void* data,UINT size,ID3D11Buffer** out) {
        D3D11_BUFFER_DESC b{};b.ByteWidth=size;b.Usage=D3D11_USAGE_IMMUTABLE;b.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
        D3D11_SUBRESOURCE_DATA initial{data,0,0};return d->CreateBuffer(&b,&initial,out);
    };
    hr=buffer(&c,sizeof(c),&constants_);if(FAILED(hr)) return hr;
    D3D11_SAMPLER_DESC sam{};sam.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sam.AddressU=sam.AddressV=sam.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;sam.MaxLOD=D3D11_FLOAT32_MAX;
    hr=d->CreateSamplerState(&sam,&sampler_);if(FAILED(hr)) return hr;
    D3D11_RASTERIZER_DESC raster{};raster.FillMode=D3D11_FILL_SOLID;raster.CullMode=D3D11_CULL_NONE;raster.DepthClipEnable=TRUE;
    hr=d->CreateRasterizerState(&raster,&rasterizer_);if(FAILED(hr)) return hr;
    {
        const bool nvidia=vendor==0x10de;
        const BYTE* code=nvidia?nis128:nis256;
        const size_t size=nvidia?sizeof(nis128):sizeof(nis256);
        hr=d->CreateComputeShader(code,size,nullptr,&compute_);if(FAILED(hr))return hr;
        NISConfig config{};
        if(!NVScalerUpdateConfig(config,nisSharpness,0,0,w,h,w,h,0,0,scaledWidth_,scaledHeight_,scaledWidth_,scaledHeight_,NISHDRMode::None)) return E_INVALIDARG;
        hr=buffer(&config,sizeof(config),&nisConstants_);if(FAILED(hr)) return hr;
        for(int i=0;i<2;i++) {
            D3D11_TEXTURE2D_DESC desc{};desc.Width=2;desc.Height=64;desc.MipLevels=desc.ArraySize=desc.SampleDesc.Count=1;
            desc.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;desc.BindFlags=D3D11_BIND_SHADER_RESOURCE;desc.Usage=D3D11_USAGE_IMMUTABLE;
            D3D11_SUBRESOURCE_DATA initial{i==0?static_cast<const void*>(coef_scale):static_cast<const void*>(coef_usm),8*sizeof(float),0};
            ComPtr<ID3D11Texture2D> tex;hr=d->CreateTexture2D(&desc,&initial,&tex);if(FAILED(hr)) return hr;
            hr=d->CreateShaderResourceView(tex.Get(),nullptr,&coefficients_[i]);if(FAILED(hr)) return hr;
        }
    }
    hr=cmaa_.Configure(d,w,h);if(FAILED(hr))return hr;
    enabled_=true;return S_OK;
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
    // CmaaPipeline::Draw binds the shared transfer vertex shader for every draw.
    c->GSSetShader(nullptr,nullptr,0);
    ID3D11ShaderResourceView* nisInput=cmaa_.Draw(c,inputView_.Get());
    ID3D11ShaderResourceView* srvs[]={nisInput,coefficients_[0].Get(),coefficients_[1].Get()};
    c->CSSetShader(compute_.Get(),nullptr,0);c->CSSetShaderResources(0,3,srvs);
    c->CSSetSamplers(0,1,sampler_.GetAddressOf());c->CSSetConstantBuffers(0,1,nisConstants_.GetAddressOf());
    c->CSSetUnorderedAccessViews(0,1,intermediateUav_.GetAddressOf(),nullptr);
    c->Dispatch((scaledWidth_+nisBlockWidth_-1)/nisBlockWidth_,(scaledHeight_+nisBlockHeight_-1)/nisBlockHeight_,1);
    ID3D11ShaderResourceView* empty[3]{};ID3D11UnorderedAccessView* uav=nullptr;
    c->CSSetShaderResources(0,3,empty);c->CSSetUnorderedAccessViews(0,1,&uav,nullptr);c->CSSetShader(nullptr,nullptr,0);
    D3D11_VIEWPORT viewport{float(destination_.left),float(destination_.top),float(scaledWidth_),float(scaledHeight_),0,1};
    c->RSSetViewports(1,&viewport);c->OMSetRenderTargets(1,&target,nullptr);
    c->PSSetConstantBuffers(0,1,constants_.GetAddressOf());
    c->PSSetShader(pixel_.Get(),nullptr,0);c->PSSetShaderResources(0,1,intermediateView_.GetAddressOf());c->Draw(3,0);
    c->PSSetShaderResources(0,1,empty);c->OMSetRenderTargets(0,nullptr,nullptr);
    c->RSSetState(nullptr);
    return S_OK;
}
}
