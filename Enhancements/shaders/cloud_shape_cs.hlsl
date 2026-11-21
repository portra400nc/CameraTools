// The volume clouds' shape noise, 128 texels a side, one channel as Frostbite keeps it: Worley fbm at four rising
// frequencies, round billows within round billows. The finer two weigh more than an fbm's halving would give them: the
// march's 60 m steps and its light samples resolve billows of 250 m and up, so the shape noise, not the erosion, has to
// carry a towering cumulus's cauliflower (.work/clouds/towers sheet-l.png). Perlin-Worley (Schneider, "The Real-time
// Volumetric Cloudscapes of Horizon Zero Dawn", 2015) made lumpy, doughy bodies in the prototype (proto.py there).
// The fbm spans only about 0.30 to 0.67, so it is stretched over 0 to 1, or the weather map's dome alone would shape a
// cloud and the billows would show only at its rim; the power rounds the billows' tops.
#include "cloud_noise.hlsli"

RWTexture3D<unorm float> shape : register(u0);

static const float kSize = 128;
static const float kLow = 0.30, kHigh = 0.67;   // its 2nd and 98th percentiles

[numthreads(4, 4, 4)]
void main(uint3 id : SV_DispatchThreadID)
{
    float3 uvw = (id + 0.5) / kSize;
    float worley = tiling_worley(uvw, 4) * 0.4 + tiling_worley(uvw, 8) * 0.25 + tiling_worley(uvw, 16) * 0.2 + tiling_worley(uvw, 32) * 0.15;
    shape[id] = pow(saturate((worley - kLow) / (kHigh - kLow)), 1.5);
}
