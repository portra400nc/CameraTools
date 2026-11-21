// The volume clouds' erosion noise, 128 texels a side, one channel: Worley fbm at five rising frequencies, cells from
// half the tile down to a thirty-second of it, which eats a cloud's edges into wisps and small billows. At 32 texels it
// reached only a sixteenth, and a towering cumulus's tile small enough for 50 m billows repeated as a lattice; real
// clouds seen from a few kilometers show billows down to some 50 m, and without them they looked like a cartoon's.
#include "cloud_noise.hlsli"

RWTexture3D<unorm float> detail : register(u0);

static const float kSize = 128;

[numthreads(4, 4, 4)]
void main(uint3 id : SV_DispatchThreadID)
{
    float3 uvw = (id + 0.5) / kSize;
    detail[id] = tiling_worley(uvw, 2) * 0.35 + tiling_worley(uvw, 4) * 0.25 + tiling_worley(uvw, 8) * 0.2 + tiling_worley(uvw, 16) * 0.12
        + tiling_worley(uvw, 32) * 0.08;
}
