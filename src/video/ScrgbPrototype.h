#pragma once
// Private diagnostic build only. One P010 -> linear scRGB draw, no RGB10
// intermediate, readback, frame queue, tone mapping, or per-frame allocation.
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <cstring>

namespace llcv::scrgb {
inline constexpr char kVideoShader[] = R"hlsl(
cbuffer Geometry : register(b0) {
    float4 destination; float4 source; float4 chromaOffset;
};
struct Vertex { float4 position : SV_POSITION; float2 uv : TEXCOORD0; };
Vertex vsMain(uint id : SV_VertexID) {
    float2 corners[4] = {float2(0,0),float2(1,0),float2(0,1),float2(1,1)};
    float2 p=corners[id]; Vertex o;
    o.position=float4(lerp(destination.xy,destination.zw,p),0,1);
    o.uv=lerp(source.xy,source.zw,p); return o;
}
Texture2D<float> luma : register(t0);
Texture2D<float2> chroma : register(t1);
SamplerState linearClamp : register(s0);
float3 pqNits(float3 v) {
    float3 p=pow(saturate(v),32.0/2523.0);
    return 10000.0*pow(max(p-3424.0/4096.0,0.0)/
        max(2413.0/128.0-2392.0/128.0*p,1e-7),16384.0/2610.0);
}
float4 psMain(Vertex v) : SV_TARGET {
    // R16_UNORM views read the full 16-bit container; P010 codes occupy bits 6..15.
    float y=(luma.Sample(linearClamp,v.uv)*(65535.0/64.0)-64.0)/876.0;
    float2 uv=(chroma.Sample(linearClamp,v.uv+chromaOffset.xy)*(65535.0/64.0)-512.0)/896.0;
    float r=y+1.4746*uv.y, b=y+1.8814*uv.x;
    float g=(y-.2627*r-.0593*b)/.6780;
    float3 nits=pqNits(float3(r,g,b));
    const float3x3 to709=float3x3(
        1.660491,-.587641,-.072850,
        -.124550,1.132900,-.008350,
        -.018151,-.100579,1.118730);
    // Never saturate linear scRGB: negative channels and >1 preserve wide gamut/HDR.
    return float4(mul(to709,nits)/80.0,1);
}
)hlsl";

inline constexpr char kOverlayShader[] = R"hlsl(
Texture2D ui : register(t0); Texture2D background : register(t1);
SamplerState uiSampler : register(s0);
cbuffer HdrUi : register(b0) { float2 origin; float whiteNits; float padding; };
float linearSrgb(float s) { return s<=.04045 ? s/12.92 : pow((s+.055)/1.055,2.4); }
float4 main(float4 position:SV_POSITION,float2 uv:TEXCOORD0):SV_TARGET {
    float4 u=ui.Sample(uiSampler,uv);
    float3 b=background.Load(int3(int2(position.xy)-int2(origin),0)).rgb;
    if(u.a<=0) return float4(b,1);
    float3 s=saturate(u.rgb/u.a);
    float3 foreground=float3(linearSrgb(s.r),linearSrgb(s.g),linearSrgb(s.b))*whiteNits/80.0;
    return float4(foreground*u.a+b*(1-u.a),1);
}
)hlsl";

struct Pipeline {
    Microsoft::WRL::ComPtr<ID3D11VertexShader> vs;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> ps;
    Microsoft::WRL::ComPtr<ID3D11Buffer> constants;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> raster;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> y[3], uv[3];
    void Reset() { *this = Pipeline{}; }
    HRESULT Initialize(ID3D11Device* device, ID3D11Texture2D* const* textures) {
        Reset();
        Microsoft::WRL::ComPtr<ID3DBlob> code, errors;
        HRESULT hr=D3DCompile(kVideoShader,strlen(kVideoShader),nullptr,nullptr,nullptr,
            "vsMain","vs_4_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&code,&errors);
        if(FAILED(hr)) return hr;
        hr=device->CreateVertexShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&vs);
        if(FAILED(hr)) return hr;
        code.Reset(); errors.Reset();
        hr=D3DCompile(kVideoShader,strlen(kVideoShader),nullptr,nullptr,nullptr,
            "psMain","ps_4_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&code,&errors);
        if(FAILED(hr)) return hr;
        hr=device->CreatePixelShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&ps);
        if(FAILED(hr)) return hr;
        for(unsigned i=0;i<3;++i) {
            D3D11_SHADER_RESOURCE_VIEW_DESC view{};
            view.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D; view.Texture2D.MipLevels=1;
            view.Format=DXGI_FORMAT_R16_UNORM;
            hr=device->CreateShaderResourceView(textures[i],&view,&y[i]);
            if(FAILED(hr)) return hr;
            view.Format=DXGI_FORMAT_R16G16_UNORM;
            hr=device->CreateShaderResourceView(textures[i],&view,&uv[i]);
            if(FAILED(hr)) return hr;
        }
        D3D11_BUFFER_DESC buffer{}; buffer.ByteWidth=48;
        buffer.Usage=D3D11_USAGE_DEFAULT; buffer.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
        hr=device->CreateBuffer(&buffer,nullptr,&constants); if(FAILED(hr)) return hr;
        D3D11_SAMPLER_DESC sample{}; sample.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sample.AddressU=sample.AddressV=sample.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;
        sample.MaxLOD=D3D11_FLOAT32_MAX;
        hr=device->CreateSamplerState(&sample,&sampler); if(FAILED(hr)) return hr;
        D3D11_RASTERIZER_DESC rs{}; rs.FillMode=D3D11_FILL_SOLID; rs.CullMode=D3D11_CULL_NONE;
        rs.DepthClipEnable=TRUE;
        return device->CreateRasterizerState(&rs,&raster);
    }
    void Configure(ID3D11DeviceContext* context, UINT width, UINT height,
                   UINT outputWidth, UINT outputHeight, RECT src, RECT dst, bool topLeft) {
        const float values[12]{
            -1+2.0f*dst.left/outputWidth,1-2.0f*dst.top/outputHeight,
            -1+2.0f*dst.right/outputWidth,1-2.0f*dst.bottom/outputHeight,
            float(src.left)/width,float(src.top)/height,float(src.right)/width,float(src.bottom)/height,
            .5f/width,topLeft ? .5f/height : 0.0f,0,0};
        context->UpdateSubresource(constants.Get(),0,nullptr,values,0,0);
    }
    HRESULT Draw(ID3D11DeviceContext* context, ID3D11RenderTargetView* target,
                 UINT width, UINT height, UINT index) {
        if(index>=3 || !ps) return E_UNEXPECTED;
        D3D11_VIEWPORT viewport{0,0,float(width),float(height),0,1};
        context->RSSetViewports(1,&viewport); context->RSSetState(raster.Get());
        context->OMSetRenderTargets(1,&target,nullptr);
        context->OMSetBlendState(nullptr,nullptr,0xffffffffu);
        context->OMSetDepthStencilState(nullptr,0);
        context->IASetInputLayout(nullptr);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        context->VSSetShader(vs.Get(),nullptr,0); context->PSSetShader(ps.Get(),nullptr,0);
        auto* cb=constants.Get(); context->VSSetConstantBuffers(0,1,&cb); context->PSSetConstantBuffers(0,1,&cb);
        auto* s=sampler.Get(); context->PSSetSamplers(0,1,&s);
        ID3D11ShaderResourceView* views[]{y[index].Get(),uv[index].Get()};
        context->PSSetShaderResources(0,2,views); context->Draw(4,0);
        ID3D11ShaderResourceView* empty[2]{}; context->PSSetShaderResources(0,2,empty);
        context->RSSetState(nullptr);
        return S_OK;
    }
};
} // namespace llcv::scrgb
