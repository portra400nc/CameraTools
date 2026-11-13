// The volume clouds' shape noise, 128 texels a side: r Perlin-Worley, the billowy body of a cloud; g, b and a Worley fbm
// at rising frequencies, which erode it.
#include "cloud_noise.hlsli"

RWTexture3D<unorm float4> shape : register(u0);

static const float kSize = 128;

[numthreads(4, 4, 4)]
void main(uint3 id : SV_DispatchThreadID)
{
    float3 uvw = (id + 0.5) / kSize;
    float perlin = saturate(tiling_perlin_fbm(uvw, 4, 4) * 0.7 + 0.5);
    float worley = tiling_worley(uvw, 4) * 0.625 + tiling_worley(uvw, 8) * 0.25 + tiling_worley(uvw, 14) * 0.125;
    float perlin_worley = worley + perlin * (1 - worley);
    shape[id] = float4(perlin_worley, worley_fbm(uvw, 8), worley_fbm(uvw, 16), tiling_worley(uvw, 32) * 0.75 + tiling_worley(uvw, 64) * 0.25);
}
