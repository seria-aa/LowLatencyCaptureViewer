// Source is raw BGR24 bytes uploaded as one byte per texel (R8_UINT,
// width = 3 * pixelWidth). Output is a native R8G8B8A8_UNORM texture -- not
// a reinterpreted/aliased view of any other format -- with an explicit
// opaque alpha so a missing fourth source byte never becomes undefined.
Texture2D<uint> sourceFrame : register(t0);
RWTexture2D<unorm float4> packedFrame : register(u0);

[numthreads(16, 16, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    uint width, height;
    packedFrame.GetDimensions(width, height);
    if (id.x >= width || id.y >= height) return;
    const uint column = id.x * 3;
    const uint b = sourceFrame.Load(int3(column, id.y, 0));
    const uint g = sourceFrame.Load(int3(column + 1, id.y, 0));
    const uint r = sourceFrame.Load(int3(column + 2, id.y, 0));
    packedFrame[id.xy] = float4(r / 255.0, g / 255.0, b / 255.0, 1.0);
}
