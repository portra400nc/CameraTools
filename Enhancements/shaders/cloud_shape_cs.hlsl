// The volume clouds' shape noise, 128 texels a side, one channel as Frostbite keeps it: Perlin-Worley, the billowy body
// of a cloud, carved by Worley fbm at three rising frequencies (Schneider, "The Real-time Volumetric Cloudscapes of
// Horizon Zero Dawn", 2015).
#include "cloud_noise.hlsli"

RWTexture3D<unorm float> shape : register(u0);

static const float kSize = 128;

[numthreads(4, 4, 4)]
void main(uint3 id : SV_DispatchThreadID)
{
    float3 uvw = (id + 0.5) / kSize;
    float perlin = saturate(tiling_perlin_fbm(uvw, 4, 4) * 0.7 + 0.5);
    float worley = tiling_worley(uvw, 4) * 0.625 + tiling_worley(uvw, 8) * 0.25 + tiling_worley(uvw, 14) * 0.125;
    float perlin_worley = worley + perlin * (1 - worley);
    float carve = worley_fbm(uvw, 8) * 0.625 + worley_fbm(uvw, 16) * 0.25 + (tiling_worley(uvw, 32) * 0.75 + tiling_worley(uvw, 64) * 0.25) * 0.125;
    shape[id] = saturate((perlin_worley - (carve - 1)) / (2 - carve));
}
