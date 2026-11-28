// The traced shot's sky map with the clouds: each of its 128 x 128 texels is the physical sky's (core/sky.h's
// physical_sky_map) through the 2x2 texels of the clouds' sky map (cloud_sky_ps.hlsl) over it, plus their light. The
// texel keeps its flag of a real texel.
Texture2D<float4> clouds : register(t0);
ByteAddressBuffer physical : register(t1);
RWByteAddressBuffer traced : register(u0);

static const uint kSide = 128;

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= kSide || id.y >= kSide)
        return;
    uint at = (id.y * kSide + id.x) * 16;
    float4 sky = asfloat(physical.Load4(at));
    float4 c = 0;
    [unroll] for (uint k = 0; k < 4; k++)
        c += clouds.Load(int3(id.xy * 2 + uint2(k & 1, k >> 1), 0));
    c *= 0.25;
    traced.Store4(at, asuint(float4(sky.rgb * (1 - saturate(c.a)) + max(c.rgb, 0), sky.w)));
}
