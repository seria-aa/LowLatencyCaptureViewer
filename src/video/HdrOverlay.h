#pragma once

namespace llcv::hdr {

// BGRA8 D2D UI is premultiplied in sRGB code space. Recover straight RGB,
// linearize, convert 709->2020, then composite in absolute linear light.
// Only the visible overlay rectangle is copied; normal video has no extra pass.
// PQ constants: ITU-R BT.2100-3 Table 4 (ST 2084 EOTF and inverse).
// The 709/sRGB -> 2020 matrix uses D65 for both spaces: no white adaptation.
inline constexpr char kOverlayShader[] = R"hlsl(
Texture2D ui : register(t0);
Texture2D background : register(t1);
SamplerState uiSampler : register(s0);
cbuffer HdrUi : register(b0) { float2 origin; float whiteNits; float padding; };
float3 pqToNits(float3 v) {
    float3 p = pow(saturate(v), 1.0 / 78.84375);
    return 10000.0 * pow(max(p - 0.8359375, 0.0) /
        max(18.8515625 - 18.6875 * p, 1e-7), 1.0 / 0.1593017578125);
}
float3 nitsToPq(float3 v) {
    float3 p = pow(saturate(v / 10000.0), 0.1593017578125);
    return pow((0.8359375 + 18.8515625 * p) / (1.0 + 18.6875 * p), 78.84375);
}
float3 srgbToLinear(float3 v) {
    return float3(v.r <= 0.04045 ? v.r / 12.92 : pow((v.r + 0.055) / 1.055, 2.4),
                  v.g <= 0.04045 ? v.g / 12.92 : pow((v.g + 0.055) / 1.055, 2.4),
                  v.b <= 0.04045 ? v.b / 12.92 : pow((v.b + 0.055) / 1.055, 2.4));
}
float4 main(float4 position : SV_POSITION, float2 uv : TEXCOORD0) : SV_TARGET {
    float4 u = ui.Sample(uiSampler, uv);
    float3 b = background.Load(int3(int2(position.xy) - int2(origin), 0)).rgb;
    if (u.a <= 0.0) return float4(b, 1.0);
    float3 linearRgb = srgbToLinear(saturate(u.rgb / u.a));
    const float3x3 to2020 = float3x3(
        0.627404, 0.329283, 0.043313,
        0.069097, 0.919540, 0.011362,
        0.016391, 0.088013, 0.895595);
    float3 foreground = mul(to2020, linearRgb) * whiteNits;
    return float4(nitsToPq(foreground * u.a + pqToNits(b) * (1.0 - u.a)), 1.0);
}
)hlsl";

} // namespace llcv::hdr
