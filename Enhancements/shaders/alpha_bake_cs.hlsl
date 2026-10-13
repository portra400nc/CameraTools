// The scene capture's atlas bake: one channel of a texture an alpha program samples, copied from the game's at its mip
// whose larger side is at most 256, resampled bilinear into its 256x256 tile of the alpha atlas, a byte a texel, four
// texels a word. One thread a word, in 8x8 groups: Dispatch(8, 32, 1) fills a tile. Registers stay inside gpu.h's
// ranges: t0, s0, u0, b0.

cbuffer Bake : register(b0)
{
    uint4 bake;     // the tile, the channel (0 = x to 3 = w), 0, 0
};

Texture2D<float4> source : register(t0);    // our copy of the game's mip, in the game's view format
SamplerState linear_clamp : register(s0);
RWByteAddressBuffer atlas : register(u0);   // 65536 bytes a tile

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    uint word = 0u;
    [unroll]
    for (uint i = 0u; i < 4u; i++)
    {
        float2 uv = (float2(id.x * 4u + i, id.y) + 0.5f) / 256.0f;
        float4 t = source.SampleLevel(linear_clamp, uv, 0.0f);
        float value = bake.y == 0u ? t.x : bake.y == 1u ? t.y : bake.y == 2u ? t.z : t.w;
        word |= uint(saturate(value) * 255.0f + 0.5f) << (i * 8u);
    }
    atlas.Store(bake.x * 65536u + id.y * 256u + id.x * 4u, word);
}
