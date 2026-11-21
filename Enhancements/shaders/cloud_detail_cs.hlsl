// The volume clouds' erosion noise, 32 texels a side, one channel: Worley fbm at three rising frequencies, which eats a
// cloud's edges into wisps and billows.
#include "cloud_noise.hlsli"

RWTexture3D<unorm float> detail : register(u0);

static const float kSize = 32;

[numthreads(4, 4, 4)]
void main(uint3 id : SV_DispatchThreadID)
{
    float3 uvw = (id + 0.5) / kSize;
    detail[id] = worley_fbm(uvw, 2) * 0.625 + worley_fbm(uvw, 4) * 0.25 + (tiling_worley(uvw, 8) * 0.75 + tiling_worley(uvw, 16) * 0.25) * 0.125;
}
