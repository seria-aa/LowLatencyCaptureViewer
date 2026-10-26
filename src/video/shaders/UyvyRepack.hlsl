// Each source uint4 stores U Y0 V Y1. The YUY2 UAV accepts Y0 U Y1 V.
// Read-only source and write-only output avoid in-place video-alias hazards.
Texture2D<uint4> sourceFrame : register(t0);
RWTexture2D<uint4> packedFrame : register(u0);

[numthreads(16, 16, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    uint width, height;
    // Only normal source dimensions are used. Video-format UAV GetDimensions
    // can report full pixel width rather than the number of packed words.
    sourceFrame.GetDimensions(width, height);
    if (id.x >= width || id.y >= height) return;
    packedFrame[id.xy] = sourceFrame.Load(int3(id.xy, 0)).yxwz;
}
