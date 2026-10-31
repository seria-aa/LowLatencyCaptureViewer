// Manual comparison only; not compiled into the viewer or installed.
// Direct3D 11 adapter of cDLAA/cFXAA from CShade.
// Upstream pin: 1e9cc77e1c2d30c22ff44a1b347747011c923954
// https://github.com/papadanku/CShade
// See CShade-LICENSE.txt and retained notices below.
// Changes: ReShade plumbing replaced by D3D11 bindings; no compositing.
// cDLAA defaults + Retain Fine Details; FXAA default thresholds + subpixel 0.25.
// Samples encoded SDR (not linear light), like upstream SampleGammaTex.
// Upstream division/saturate behavior is retained; comparisons are not HDR claims.
cbuffer Comparison : register(b0) { float2 InvSize; float DlaaStrength; float Padding; };
Texture2D<float4> Source : register(t0);
Texture2D<float4> Prefilter : register(t1);
SamplerState ClampLinear : register(s0);
SamplerState MirrorLinear : register(s1);
struct CShade_VS2PS_Quad { float4 HPos : SV_POSITION; float2 Tex0 : TEXCOORD0; };
float4 CMath_PadFloat2(float2 p) { return float4(p,0,0); }
float CMath_UNORMtoSNORM_FLT1(float x) { return x*2-1; }
float2 CMath_SNORMtoUNORM_FLT2(float2 x) { return x*.5+.5; }
static const float3 CColor_Rec709_Coefficients=float3(.2126,.7152,.0722);
float4 Gamma(float4 p) { return Source.SampleLevel(ClampLinear,p.xy,0); }
float4 Filtered(float4 p) { return Prefilter.SampleLevel(MirrorLinear,p.xy,0); }
CShade_VS2PS_Quad VS_Quad(uint id : SV_VERTEXID) {
    CShade_VS2PS_Quad o; o.Tex0=float2((id<<1)&2,id&2);
    o.HPos=float4(o.Tex0*float2(2,-2)+float2(-1,1),0,1);return o;
}
float4 PS_Copy(CShade_VS2PS_Quad i) : SV_TARGET { return Gamma(float4(i.Tex0,0,0)); }
#if CANDIDATE_DLAA
#define CSHADE_DLAA

/*
    This shader implements Directionally Localized Anti-Aliasing (DLAA). It reduces aliasing artifacts by detecting and smoothing edges while preserving fine details. The shader differentiates between short and long edges, applying specialized processing based on configurable contrast thresholds. It also includes an option to retain high frequencies and display edge masks for visualization.
*/

/*
    Directionally Localized Anti-Aliasing (DLAA)
    http://www.and.intercon.ru/releases/talks/dlaagdc2011/

    by Dmitry Andreev
    Copyright (C) LucasArts 2010-2011
*/



/* Shader Options */

static const bool _PreserveFrequencies = true;

static const int _DisplayMode = 0;

static const int _ShortEdgesScale = 3.0;

static const int _ShortEdgesContrastThreshold = 2;

static const int _LongEdgesContrastThreshold = 3;

static const float ContrastThresholds[6] =
{
    0.0, 1.0 / 16.0, 1.0 / 8.0, 1.0 / 6.0, 1.0 / 4.0, 1.0 / 3.0
};

#define CSHADE_APPLY_AUTO_EXPOSURE 0
#define CSHADE_APPLY_ABBERATION 0





float GetIntensity(float3 Color)
{
    return dot(Color, 1.0 / 3.0);
}

void PS_Prefilter(CShade_VS2PS_Quad Input, out float4 Output : SV_TARGET0)
{
    float2 Delta = InvSize;
    float4 EdgeTex0 = Input.Tex0.xyxy + (float4(-1.0, 0.0, 1.0, 0.0) * Delta.xyxy);
    float4 EdgeTex1 = Input.Tex0.xyxy + (float4(0.0, -1.0, 0.0, 1.0) * Delta.xyxy);

    float3 Neighborhood[4];
    float3 Center = Gamma(CMath_PadFloat2(Input.Tex0)).rgb;
    Neighborhood[0] = Gamma(CMath_PadFloat2(EdgeTex0.xy)).rgb;
    Neighborhood[1] = Gamma(CMath_PadFloat2(EdgeTex0.zw)).rgb;
    Neighborhood[2] = Gamma(CMath_PadFloat2(EdgeTex1.xy)).rgb;
    Neighborhood[3] = Gamma(CMath_PadFloat2(EdgeTex1.zw)).rgb;

    // Compass edge detection on N/S/E/W
    float3 Edges = 0.0;
    Edges = max(Edges, abs(Center - Neighborhood[0]));
    Edges = max(Edges, abs(Center - Neighborhood[1]));
    Edges = max(Edges, abs(Center - Neighborhood[2]));
    Edges = max(Edges, abs(Center - Neighborhood[3]));

    Output.rgb = Center;
    Output.a = smoothstep(0.0, 0.25, GetIntensity(Edges));
}

void PS_Main(CShade_VS2PS_Quad Input, out float4 Output : SV_TARGET0)
{
    float2 Delta = InvSize;

    float4 Center = Gamma(CMath_PadFloat2(Input.Tex0));

    float4 ShortTex1 = Input.Tex0.xyxy + (float4(-1.0, 0.0, 1.0, 0.0) * Delta.xyxy);
    float4 ShortTex2 = Input.Tex0.xyxy + (float4(0.0, -1.0, 0.0, 1.0) * Delta.xyxy);

    float4 Left = Filtered(CMath_PadFloat2(ShortTex1.xy));
    float4 Right = Filtered(CMath_PadFloat2(ShortTex1.zw));
    float4 Top = Filtered(CMath_PadFloat2(ShortTex2.xy));
    float4 Bottom = Filtered(CMath_PadFloat2(ShortTex2.zw));

    float4 LongTex0 = Input.Tex0.xyxy + (float4(1.5, 0.0, 0.0, 1.5) * Delta.xyxy);
    float4 LongTex1 = Input.Tex0.xyxy + (float4(3.5, 0.0, 0.0, 3.5) * Delta.xyxy);
    float4 LongTex2 = Input.Tex0.xyxy + (float4(5.5, 0.0, 0.0, 5.5) * Delta.xyxy);
    float4 LongTex3 = Input.Tex0.xyxy + (float4(7.5, 0.0, 0.0, 7.5) * Delta.xyxy);
    float4 LongTex4 = Input.Tex0.xyxy + (float4(-1.5, 0.0, 0.0, -1.5) * Delta.xyxy);
    float4 LongTex5 = Input.Tex0.xyxy + (float4(-3.5, 0.0, 0.0, -3.5) * Delta.xyxy);
    float4 LongTex6 = Input.Tex0.xyxy + (float4(-5.5, 0.0, 0.0, -5.5) * Delta.xyxy);
    float4 LongTex7 = Input.Tex0.xyxy + (float4(-7.5, 0.0, 0.0, -7.5) * Delta.xyxy);

    float4 H0 = Filtered(CMath_PadFloat2(LongTex0.xy));
    float4 H1 = Filtered(CMath_PadFloat2(LongTex1.xy));
    float4 H2 = Filtered(CMath_PadFloat2(LongTex2.xy));
    float4 H3 = Filtered(CMath_PadFloat2(LongTex3.xy));
    float4 H4 = Filtered(CMath_PadFloat2(LongTex4.xy));
    float4 H5 = Filtered(CMath_PadFloat2(LongTex5.xy));
    float4 H6 = Filtered(CMath_PadFloat2(LongTex6.xy));
    float4 H7 = Filtered(CMath_PadFloat2(LongTex7.xy));

    float4 V0 = Filtered(CMath_PadFloat2(LongTex0.zw));
    float4 V1 = Filtered(CMath_PadFloat2(LongTex1.zw));
    float4 V2 = Filtered(CMath_PadFloat2(LongTex2.zw));
    float4 V3 = Filtered(CMath_PadFloat2(LongTex3.zw));
    float4 V4 = Filtered(CMath_PadFloat2(LongTex4.zw));
    float4 V5 = Filtered(CMath_PadFloat2(LongTex5.zw));
    float4 V6 = Filtered(CMath_PadFloat2(LongTex6.zw));
    float4 V7 = Filtered(CMath_PadFloat2(LongTex7.zw));

    /*
        Short edges
    */

    float CenterLuma = GetIntensity(Center.rgb);
    float LeftLuma = GetIntensity(Left.rgb);
    float RightLuma = GetIntensity(Right.rgb);
    float TopLuma = GetIntensity(Top.rgb);
    float BottomLuma = GetIntensity(Bottom.rgb);

    // 3-pixel wide high-pass
    float4 EdgeH = abs((Left + Right) - (2.0 * Center)) / 2.0;
    float4 EdgeV = abs((Top + Bottom) - (2.0 * Center)) / 2.0;

    // Get low-pass
    float4 WH = 2.0 * (H4 + H0);
    float4 WV = 2.0 * (V0 + V4);
    float4 BlurH = (WH + (2.0 * Center)) / 6.0;
    float4 BlurV = (WV + (2.0 * Center)) / 6.0;

    // Get respective intensities
    float EdgeLumaH = GetIntensity(EdgeH.rgb);
    float EdgeLumaV = GetIntensity(EdgeV.rgb);

    float MaxLumaH = max(CenterLuma, max(LeftLuma, RightLuma));
    float MinLumaH = min(CenterLuma, min(LeftLuma, RightLuma));
    float RangeLumaH = MaxLumaH - MinLumaH;

    float MaxLumaV = max(CenterLuma, max(TopLuma, BottomLuma));
    float MinLumaV = min(CenterLuma, min(TopLuma, BottomLuma));
    float RangeLumaV = MaxLumaV - MinLumaV;

    float BlurLumaH = GetIntensity(BlurH.rgb);
    float BlurLumaV = GetIntensity(BlurV.rgb);

    // Edge masks
    float EdgeMaskH = saturate((_ShortEdgesScale * EdgeLumaH - ContrastThresholds[_ShortEdgesContrastThreshold]) / RangeLumaV);
    float EdgeMaskV = saturate((_ShortEdgesScale * EdgeLumaV - ContrastThresholds[_ShortEdgesContrastThreshold]) / RangeLumaH);

    float4 Color = Center;
    Color = lerp(Color, BlurH, EdgeMaskV);
    Color = lerp(Color, BlurV, EdgeMaskH);

    /*
        Long edges
    */

    // In CShade, we take .rgb out of branch
    float4 LongBlurH = (H0 + H1 + H2 + H3 + H4 + H5 + H6 + H7) / 8.0;
    float4 LongBlurV = (V0 + V1 + V2 + V3 + V4 + V5 + V6 + V7) / 8.0;

    float LongEdgeMaskH = saturate(CMath_UNORMtoSNORM_FLT1(LongBlurH.a));
    float LongEdgeMaskV = saturate(CMath_UNORMtoSNORM_FLT1(LongBlurV.a));

    // Put the criteria into a variable.
    bool IsLongEdge = abs(LongEdgeMaskH - LongEdgeMaskV) > ContrastThresholds[_LongEdgesContrastThreshold];

    // This is for the "Long Edge Mask" Render Mode.
    float LongEdgeMaskDebugH = LongEdgeMaskH * IsLongEdge;
    float LongEdgeMaskDebugV = LongEdgeMaskV * IsLongEdge;

    if (IsLongEdge)
    {
        float LongBlurLumaH = GetIntensity(LongBlurH.rgb);
        float LongBlurLumaV = GetIntensity(LongBlurV.rgb);

        float4 ColorH = Center;
        float4 ColorV = Center;

        // Vectorized search
        float HX = saturate(0.0 + (LongBlurLumaH - TopLuma) / (CenterLuma - TopLuma));
        float HY = saturate(1.0 + (LongBlurLumaH - CenterLuma) / (CenterLuma - BottomLuma));
        float VX = saturate(0.0 + (LongBlurLumaV - LeftLuma) / (CenterLuma - LeftLuma));
        float VY = saturate(1.0 + (LongBlurLumaV - CenterLuma) / (CenterLuma - RightLuma));

        float4 VHXY = float4(VX, VY, HX, HY);
        VHXY = (VHXY == 0.0) ? 1.0 : VHXY;

        ColorV = lerp(Left, ColorV, VHXY.x);
        ColorV = lerp(Right, ColorV, VHXY.y);
        ColorH = lerp(Top, ColorH, VHXY.z);
        ColorH = lerp(Bottom, ColorH, VHXY.w);

        Color = lerp(Color, ColorV, LongEdgeMaskV);
        Color = lerp(Color, ColorH, LongEdgeMaskH);
    }

    // Preserve high frequencies
    if (_PreserveFrequencies)
    {
        float4 RTex = Input.Tex0.xyxy + (Delta.xyxy * float4(-1.5, -1.5, 1.5, 1.5));
        float4 R0 = Filtered(CMath_PadFloat2(RTex.xw));
        float4 R1 = Filtered(CMath_PadFloat2(RTex.zw));
        float4 R2 = Filtered(CMath_PadFloat2(RTex.xy));
        float4 R3 = Filtered(CMath_PadFloat2(RTex.zy));

        float4 R = (4.0 * (R0 + R1 + R2 + R3) + Center + V0 + V4 + H4 + H0) / 25.0;
        Color = lerp(Color, Center, saturate(R.a * 3.0 - 1.5));
    }

    // Test-only strength control: blend the completed AA correction with the
    // unmodified native-resolution pixel, before NIS. No extra sharpening.
    Color = lerp(Center, Color, DlaaStrength);

    switch (_DisplayMode)
    {
        case 1:
            Color.rgb = float3(EdgeMaskH, EdgeMaskV, 0.0);
            break;
        case 2:
            Color.rgb = float3(LongEdgeMaskDebugH, LongEdgeMaskDebugV, 0.0);
            break;
    }

    // RENDER
    #if defined(CSHADE_BLENDING)
        Output = float4(Color.rgb, _CShade_AlphaFactor);
    #else
        Output = float4(Color.rgb, 1.0);
    #endif

}

#else
#define CSHADE_FXAA

/*
    This shader implements Fast Approximate Anti-Aliasing (FXAA) to reduce jagged edges in images. Operating in a single pass, it detects edges based on luma contrast and then blends pixels along those edges for a smoothing effect. The shader offers adjustable parameters for relative contrast threshold, edge contrast threshold, and subpixel blending strength, allowing users to fine-tune the anti-aliasing. It also includes a debug display mode to visualize detected edge directions.
*/

/*
    Single-Pass FXAA modification of:
        - https://bitbucket.org/catlikecodingunitytutorials/custom-srp-17-fxaa/src
        - https://catlikecoding.com/unity/tutorials/custom-srp/fxaa/

    MIT No Attribution (MIT-0)

    Copyright 2021 Jasper Flick

    Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
*/



static const int _DisplayMode = 0;

static const int _RelativeThreshold = 1;

static const float RelativeThresholds[3] =
{
    1.0 / 12.0, 1.0 / 16.0, 1.0 / 32.0
};

static const int _ContrastThreshold = 2;

static const float ContrastThresholds[5] =
{
    1.0 / 3.0, 1.0 / 4.0, 1.0 / 6.0, 1.0 / 8.0, 1.0 / 16.0
};

static const int _SubpixelBlending = 3;

static const float SubpixelBlendings[5] =
{
    1.0, 3.0 / 4.0, 1.0 / 2.0, 1.0 / 4.0, 0.0
};

#define CSHADE_APPLY_AUTO_EXPOSURE 0
#define CSHADE_APPLY_ABBERATION 0


float SampleLuma(float2 Tex, float2 Offset, float2 Delta)
{
    float4 Tex1 = CMath_PadFloat2(Tex + (Offset * Delta));
    float3 Color = Gamma(Tex1).rgb;
    return dot(Color, CColor_Rec709_Coefficients);
}

struct LumaNeighborhood
{
    float4 C;
    float M, N, E, S, W;
    float Highest, Lowest, Range;
};

LumaNeighborhood GetLumaNeighborhood(float2 Tex, float2 Delta)
{
    LumaNeighborhood L;
    L.C = Gamma(CMath_PadFloat2(Tex));
    L.M = dot(L.C.rgb, CColor_Rec709_Coefficients);
    L.N = SampleLuma(Tex, float2(0.0, 1.0), Delta);
    L.E = SampleLuma(Tex, float2(1.0, 0.0), Delta);
    L.S = SampleLuma(Tex, float2(0.0, -1.0), Delta);
    L.W = SampleLuma(Tex, float2(-1.0, 0.0), Delta);
    L.Highest = max(max(max(max(L.M, L.N), L.E), L.S), L.W);
    L.Lowest = min(min(min(min(L.M, L.N), L.E), L.S), L.W);
    L.Range = L.Highest - L.Lowest;
    return L;
}

struct LumaDiagonals
{
    float NE, SE, SW, NW;
};

LumaDiagonals GetLumaDiagonals(float2 Tex, float2 Delta)
{
    LumaDiagonals L;
    L.NE = SampleLuma(Tex, float2(1.0, 1.0), Delta);
    L.SE = SampleLuma(Tex, float2(1.0, -1.0), Delta);
    L.SW = SampleLuma(Tex, float2(-1.0, -1.0), Delta);
    L.NW = SampleLuma(Tex, float2(-1.0, 1.0), Delta);
    return L;
}

float GetSubpixelBlendFactor(LumaNeighborhood LN, LumaDiagonals LD)
{
    float Blend = 2.0 * (LN.N + LN.E + LN.S + LN.W);
    Blend += LD.NE + LD.NW + LD.SE + LD.SW;
    Blend *= (1.0 / 12.0);
    Blend = abs(Blend - LN.M);
    Blend = saturate(Blend / LN.Range);
    Blend = smoothstep(0.0, 1.0, Blend);
    return Blend * Blend * SubpixelBlendings[_SubpixelBlending];
}

bool IsHorizontalEdge(LumaNeighborhood LN, LumaDiagonals LD)
{
    float Horizontal = 0.0;
    Horizontal += 2.0 * abs(LN.N + LN.S - (2.0 * LN.M));
    Horizontal += abs(LD.NE + LD.SE - (2.0 * LN.E));
    Horizontal += abs(LD.NW + LD.SW - (2.0 * LN.W));
    float Vertical = 0.0;
    Vertical += 2.0 * abs(LN.E + LN.W - (2.0 * LN.M));
    Vertical += abs(LD.NE + LD.NW - (2.0 * LN.N));
    Vertical += abs(LD.SE + LD.SW - (2.0 * LN.S));
    return Horizontal >= Vertical;
}

bool SkipFXAA(LumaNeighborhood LN)
{
    return LN.Range < max(RelativeThresholds[_RelativeThreshold], ContrastThresholds[_ContrastThreshold] * LN.Highest);
}

struct Edge
{
    bool IsHorizontal;
    float PixelStep;
    float LumaGradient, OtherLuma;
};

Edge GetEdge(LumaNeighborhood LN, LumaDiagonals LD, float2 Delta)
{
    Edge E;
    E.IsHorizontal = IsHorizontalEdge(LN, LD);
    E.PixelStep = (E.IsHorizontal) ? Delta.y : Delta.x;
    float LumaP = (E.IsHorizontal) ? LN.N : LN.E;
    float LumaN = (E.IsHorizontal) ? LN.S : LN.W;

    float GradientP = abs(LumaP - LN.M);
    float GradientN = abs(LumaN - LN.M);
    if (GradientP < GradientN)
    {
        E.PixelStep = -E.PixelStep;
        E.LumaGradient = GradientN;
        E.OtherLuma = LumaN;
    }
    else
    {
        E.LumaGradient = GradientP;
        E.OtherLuma = LumaP;
    }

    return E;
}

float GetEdgeBlendFactor(LumaNeighborhood LN, LumaDiagonals LD, Edge E, float2 Tex, float2 Delta)
{
    const int EdgeStepCount = 7;
    const float EdgeSteps[EdgeStepCount] = { 1.0, 1.5, 2.0, 2.0, 2.0, 2.0, 4.0 };
    const float LastStep = 12.0;

    float2 EdgeTex = Tex;
    float2 TexStep = 0.0;
    if (E.IsHorizontal)
    {
        EdgeTex.y += (E.PixelStep * 0.5);
        TexStep.x = Delta.x;
    }
    else
    {
        EdgeTex.x += (E.PixelStep * 0.5);
        TexStep.y = Delta.y;
    }

    // Precompute this
    float EdgeLuma = 0.5 * (LN.M + E.OtherLuma);
    float GradientThreshold = 0.25 * E.LumaGradient;

    // March in the positive direction
    float2 TexP = EdgeTex + TexStep;
    float LumaDeltaP = SampleLuma(TexP, 0.0, Delta) - EdgeLuma;
    bool AtEndP = abs(LumaDeltaP) >= GradientThreshold;
    [unroll]
    for (int i = 0; i < EdgeStepCount && !AtEndP; i++)
    {
        TexP += (TexStep * EdgeSteps[i]);
        LumaDeltaP = SampleLuma(TexP, 0.0, Delta) - EdgeLuma;
        AtEndP = abs(LumaDeltaP) >= GradientThreshold;
    }
    if (!AtEndP)
    {
        TexP += (TexStep * LastStep);
    }

    // March in the negative direction
    float2 TexN = EdgeTex - TexStep;
    float LumaDeltaN = SampleLuma(TexN, 0.0, Delta) - EdgeLuma;
    bool AtEndN = abs(LumaDeltaN) >= GradientThreshold;
    [unroll]
    for (int j = 0; j < EdgeStepCount && !AtEndN; j++)
    {
        TexN -= (TexStep * EdgeSteps[j]);
        LumaDeltaN = SampleLuma(TexN, 0.0, Delta) - EdgeLuma;
        AtEndN = abs(LumaDeltaN) >= GradientThreshold;
    }
    if (!AtEndN)
    {
        TexN -= (TexStep * LastStep);
    }

    float DistanceToEndP = (E.IsHorizontal) ? TexP.x - Tex.x : TexP.y - Tex.y;
    float DistanceToEndN = (E.IsHorizontal) ? Tex.x - TexN.x : Tex.y - TexN.y;

    float DistanceToNearestEnd = 0.0;
    float DeltaSign = 0.0;

    if (DistanceToEndP <= DistanceToEndN)
    {
        DistanceToNearestEnd = DistanceToEndP;
        DeltaSign = LumaDeltaP >= 0;
    }
    else
    {
        DistanceToNearestEnd = DistanceToEndN;
        DeltaSign = LumaDeltaN >= 0;
    }

    if (DeltaSign == (LN.M - EdgeLuma >= 0))
    {
        return 0.0;
    }
    else
    {
        return 0.5 - DistanceToNearestEnd / (DistanceToEndP + DistanceToEndN);
    }
}

void PS_Main(CShade_VS2PS_Quad Input, out float4 Output : SV_TARGET0)
{
    float2 Delta = InvSize;
    LumaNeighborhood LN = GetLumaNeighborhood(Input.Tex0, Delta);
    float3 FXAA = 0.0;
    float2 BlendTex = Input.Tex0;

    [branch]
    if (SkipFXAA(LN))
    {
        FXAA = LN.C.rgb;
    }
    else
    {
        LumaDiagonals LD = GetLumaDiagonals(Input.Tex0, Delta);
        Edge E = GetEdge(LN, LD, Delta);

        float SubpixelBlendFactor = GetSubpixelBlendFactor(LN, LD);
        float EdgeBlendFactor = GetEdgeBlendFactor(LN, LD, E, Input.Tex0, Delta);
        float BlendFactor = max(SubpixelBlendFactor, EdgeBlendFactor);

        if (E.IsHorizontal)
        {
            BlendTex.y += (BlendFactor * E.PixelStep);
        }
        else
        {
            BlendTex.x += (BlendFactor * E.PixelStep);
        }

        FXAA = Gamma(CMath_PadFloat2(BlendTex)).rgb;
    }

    // RENDER
    #if defined(CSHADE_BLENDING)
        Output = float4(FXAA.rgb, _CShade_AlphaFactor);
    #else
        Output = float4(FXAA.rgb, 1.0);
    #endif


    if (_DisplayMode == 1)
    {
        Output.rgb = float3(Input.Tex0 - BlendTex, 0.0);
        Output.rg = CMath_SNORMtoUNORM_FLT2(normalize(Output.rg));
    }
}

#endif
