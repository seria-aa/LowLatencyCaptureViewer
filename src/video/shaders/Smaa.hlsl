// Private D3D11 binding wrapper; official SMAA math and LUTs are unmodified.
// SMAA 1x HIGH/luma or ULTRA/color; no predication/reprojection or jitter.
cbuffer SmaaMetrics : register(b0) { float4 metrics; }
SamplerState linearClamp : register(s0);
SamplerState pointClamp : register(s1);
Texture2D tex0 : register(t0);
Texture2D tex1 : register(t1);
Texture2D tex2 : register(t2);
#define SMAA_RT_METRICS metrics
#if SMAA_ULTRA_COLOR
#define SMAA_PRESET_ULTRA 1
#else
#define SMAA_PRESET_HIGH 1
#endif
#define SMAA_CUSTOM_SL 1
#define SMAATexture2D(tex) Texture2D tex
#define SMAATexturePass2D(tex) tex
#define SMAASampleLevelZero(tex, coord) tex.SampleLevel(linearClamp, coord, 0)
#define SMAASampleLevelZeroPoint(tex, coord) tex.SampleLevel(pointClamp, coord, 0)
#define SMAASampleLevelZeroOffset(tex, coord, offset) tex.SampleLevel(linearClamp, coord, 0, offset)
#define SMAASample(tex, coord) tex.Sample(linearClamp, coord)
#define SMAASamplePoint(tex, coord) tex.Sample(pointClamp, coord)
#define SMAASampleOffset(tex, coord, offset) tex.Sample(linearClamp, coord, offset)
#define SMAA_FLATTEN [flatten]
#define SMAA_BRANCH [branch]
#include "../../../third_party/upscaling/smaa/SMAA.hlsl"
struct Varying {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    float4 offset[3] : TEXCOORD1;
    float2 pixel : TEXCOORD4;
};
Varying Vertex(uint id : SV_VertexID) {
    Varying o=(Varying)0;
    o.uv=float2((id<<1)&2,id&2);
    o.position=float4(o.uv*float2(2,-2)+float2(-1,1),0,1);
#if SMAA_PASS == 0
    SMAAEdgeDetectionVS(o.uv,o.offset);
#elif SMAA_PASS == 1
    SMAABlendingWeightCalculationVS(o.uv,o.pixel,o.offset);
#else
    SMAANeighborhoodBlendingVS(o.uv,o.offset[0]);
#endif
    return o;
}
float4 Pixel(Varying i) : SV_Target {
#if SMAA_PASS == 0
#if SMAA_ULTRA_COLOR
    return float4(SMAAColorEdgeDetectionPS(i.uv,i.offset,tex0),0,0);
#else
    return float4(SMAALumaEdgeDetectionPS(i.uv,i.offset,tex0),0,0);
#endif
#elif SMAA_PASS == 1
    return SMAABlendingWeightCalculationPS(i.uv,i.pixel,i.offset,tex0,tex1,tex2,float4(0,0,0,0));
#else
    // All buffers are non-sRGB. The official supported gamma-space resolve
    // avoids imposing an sRGB transfer on the capture's BT.709-encoded RGB.
    return float4(SMAANeighborhoodBlendingPS(i.uv,i.offset[0],tex0,tex1).rgb,1);
#endif
}
