// Private D3D11 wrappers. Vendor kernels remain unmodified under third_party.
Texture2D<float4> source : register(t0);
SamplerState linearClamp : register(s0);
cbuffer Settings : register(b0) {
    uint4 con0, con1, con2, con3;
    uint4 rcas;
    float4 dimensions; // source width/height, destination width/height
    float4 placement;  // destination x/y
};
float4 Vertex(uint id : SV_VertexID) : SV_Position {
    float2 p = float2((id << 1) & 2, id & 2);
    return float4(p * float2(2,-2) + float2(-1,1), 0, 1);
}
int2 ClampPixel(int2 p, float2 size) { return clamp(p, int2(0,0), int2(size)-1); }

#if defined(METHOD_CAS)
#define A_GPU 1
#define A_HLSL 1
#include "../../../third_party/upscaling/cas/ffx_a.h"
AF3 CasLoad(ASU2 p) { return source.Load(int3(ClampPixel(p,dimensions.xy),0)).rgb; }
// Official CAS recommended gamma-2 approximation; linear processing, gamma output.
void CasInput(inout AF1 r, inout AF1 g, inout AF1 b) { r*=r; g*=g; b*=b; }
#include "../../../third_party/upscaling/cas/ffx_cas.h"
float4 Pixel(float4 pos : SV_Position) : SV_Target {
    float3 c; CasFilter(c.r,c.g,c.b,uint2(pos.xy-placement.xy),con0,con1,false);
    return float4(sqrt(saturate(c)),1);
}
#elif defined(METHOD_FSR) || defined(METHOD_RCAS)
#define A_GPU 1
#define A_HLSL 1
#if defined(METHOD_FSR)
#define FSR_EASU_F 1
#else
#define FSR_RCAS_F 1
#endif
#include "../../../third_party/upscaling/fsr/ffx_a.h"
#include "../../../third_party/upscaling/fsr/ffx_fsr1.h"
#if defined(METHOD_FSR)
AF4 FsrEasuRF(AF2 p) { return source.GatherRed(linearClamp,p); }
AF4 FsrEasuGF(AF2 p) { return source.GatherGreen(linearClamp,p); }
AF4 FsrEasuBF(AF2 p) { return source.GatherBlue(linearClamp,p); }
float4 Pixel(float4 pos : SV_Position) : SV_Target {
    float3 c; FsrEasuF(c,uint2(pos.xy),con0,con1,con2,con3); return float4(saturate(c),1);
}
#else
AF4 FsrRcasLoadF(ASU2 p) { return source.Load(int3(ClampPixel(p,dimensions.zw),0)); }
void FsrRcasInputF(inout AF1 r,inout AF1 g,inout AF1 b) {}
float4 Pixel(float4 pos : SV_Position) : SV_Target {
    float3 c; FsrRcasF(c.r,c.g,c.b,uint2(pos.xy-placement.xy),rcas); return float4(saturate(c),1);
}
#endif
#elif defined(METHOD_SGSR)
#define SGSR_MOBILE 1
#define SGSR_H 1
half4 SGSRRGBH(float2 p) { return source.SampleLevel(linearClamp,p,0); }
half4 SGSRH(float2 p,uint channel) {
    if(channel==0)return source.GatherRed(linearClamp,p);
    if(channel==1)return source.GatherGreen(linearClamp,p);
    if(channel==2)return source.GatherBlue(linearClamp,p);
    return source.GatherAlpha(linearClamp,p);
}
#include "../../../third_party/upscaling/sgsr/sgsr1_mobile.h"
float4 Pixel(float4 pos : SV_Position) : SV_Target {
    half4 c;SgsrYuvH(c,(pos.xy-placement.xy)/dimensions.zw,float4(1/dimensions.xy,dimensions.xy));
    return float4(c.rgb,1);
}
#elif defined(METHOD_JINC) || defined(METHOD_LANCZOS)
// Windowed-sinc reconstruction. Central 2x2 anti-ringing blend = 0.5.
// Jinc2 uses the standard two-sine radial approximation A=.5, B=.825.
// Unlike Magpie's pinned Jinc port, every last-row tap is unique (no duplicated
// src[2][3] in place of src[1][3]). Lanczos3 widens its support when reducing
// the FSRCNNX 2x result, so the extra downsample is low-pass filtered.
float Sinc(float x) { return abs(x)<1e-5 ? 1 : sin(3.141592653589793*x)/(3.141592653589793*x); }
float4 Pixel(float4 pos : SV_Position) : SV_Target {
    float2 p=(pos.xy-placement.xy)*dimensions.xy/dimensions.zw-.5;
    int2 origin=int2(floor(p));
    float3 color=0,lo=1,hi=0;float sum=0;
#if defined(METHOD_JINC)
    int2 radius=int2(2,2);float2 scale=1;
#else
    float2 scale=max(1,dimensions.xy/dimensions.zw);
    int2 radius=int2(ceil(3*scale));
#endif
    [loop] for(int y=1-radius.y;y<=radius.y;y++) [loop] for(int x=1-radius.x;x<=radius.x;x++) {
        float2 delta=(p-float2(origin+int2(x,y)))/scale;float weight;
#if defined(METHOD_JINC)
        float distance=length(delta);
        weight=Sinc(distance*.5)*Sinc(distance*.825);
#else
        weight=(abs(delta.x)<3 && abs(delta.y)<3)?Sinc(delta.x)*Sinc(delta.x/3)*Sinc(delta.y)*Sinc(delta.y/3):0;
#endif
        float3 sample=source.Load(int3(ClampPixel(origin+int2(x,y),dimensions.xy),0)).rgb;
        color+=sample*weight;sum+=weight;
        if(x>=0 && x<=1 && y>=0 && y<=1){lo=min(lo,sample);hi=max(hi,sample);}
    }
    color/=max(sum,1e-6);
    return float4(saturate(lerp(color,clamp(color,lo,hi),.5)),1);
}
#elif defined(METHOD_BICUBIC)
// Catmull-Rom, 16 taps, clamped at texture boundaries. No sharpening pass.
float4 Weights(float t) {
    float t2=t*t, t3=t2*t;
    return float4(-.5*t+t2-.5*t3, 1-2.5*t2+1.5*t3,
                  .5*t+2*t2-1.5*t3, -.5*t2+.5*t3);
}
float4 Pixel(float4 pos : SV_Position) : SV_Target {
    float2 p=(pos.xy-placement.xy)*dimensions.xy/dimensions.zw-.5;
    int2 origin=int2(floor(p)); float4 wx=Weights(frac(p.x)),wy=Weights(frac(p.y));
    float3 c=0;
    [unroll] for(int y=0;y<4;y++) [unroll] for(int x=0;x<4;x++)
        c+=source.Load(int3(ClampPixel(origin+int2(x-1,y-1),dimensions.xy),0)).rgb*wx[x]*wy[y];
    return float4(saturate(c),1);
}
#else
float4 Pixel(float4 pos : SV_Position) : SV_Target {
    return float4(source.Load(int3(int2(pos.xy-placement.xy),0)).rgb,1);
}
#endif
