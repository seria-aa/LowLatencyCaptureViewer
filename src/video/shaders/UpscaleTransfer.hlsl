// SDR only: linearize BT.709 while blending CMAA2 edges, then restore encoding.
Texture2D<float4> source : register(t0);
cbuffer Settings : register(b0) {
    float4 placement;
};
float4 Vertex(uint id : SV_VertexID) : SV_Position {
    float2 p=float2((id<<1)&2,id&2);
    return float4(p*float2(2,-2)+float2(-1,1),0,1);
}
float Decode709(float v) { return v<0.081?v/4.5:pow((v+0.099)/1.099,1.0/0.45); }
float Encode709(float v) { return v<0.018?v*4.5:1.099*pow(v,0.45)-0.099; }
float4 Decode(float4 pos : SV_Position) : SV_Target {
    float3 c=saturate(source.Load(int3(pos.xy,0)).rgb);
    return float4(Decode709(c.r),Decode709(c.g),Decode709(c.b),1);
}
float4 Encode(float4 pos : SV_Position) : SV_Target {
    float3 c=source.Load(int3(pos.xy,0)).rgb;
    c=saturate(c);
    return float4(Encode709(c.r),Encode709(c.g),Encode709(c.b),1);
}
float4 Copy(float4 pos : SV_Position) : SV_Target {
    return float4(source.Load(int3(int2(pos.xy-placement.xy),0)).rgb,1);
}
