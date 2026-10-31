// Legacy HDR AA reference, compiled only for offline comparison tests.
Texture2D<float4> source : register(t0);
float4 DecodePq(float4 pos : SV_Position) : SV_Target {
    float3 p=pow(saturate(source.Load(int3(pos.xy,0)).rgb),1.0/78.84375);
    float3 light=125.0*pow(max(p-0.8359375,0.0)/max(18.8515625-18.6875*p,1e-7),1.0/0.1593017578125);
    return float4(light,1);
}
float4 EncodePq(float4 pos : SV_Position) : SV_Target {
    float3 p=pow(saturate(source.Load(int3(pos.xy,0)).rgb/125.0),0.1593017578125);
    return float4(pow((0.8359375+18.8515625*p)/(1.0+18.6875*p),78.84375),1);
}
