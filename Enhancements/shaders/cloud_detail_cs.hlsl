// The volume clouds' detail noise, 32 texels a side: Worley fbm at three frequencies, which eat the edges into wisps and
// billows.
#include "cloud_noise.hlsli"

RWTexture3D<unorm float4> detail : register(u0);

static const float kSize = 32;

[numthreads(4, 4, 4)]
void main(uint3 id : SV_DispatchThreadID)
{
    float3 uvw = (id + 0.5) / kSize;
    detail[id] = float4(worley_fbm(uvw, 2), worley_fbm(uvw, 4), tiling_worley(uvw, 8) * 0.75 + tiling_worley(uvw, 16) * 0.25, 1);
}
