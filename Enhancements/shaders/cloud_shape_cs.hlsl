// The volume clouds' shape noise, 128 texels a side, one channel as Frostbite keeps it: Worley fbm at three rising
// frequencies, round billows within round billows. Perlin-Worley (Schneider, "The Real-time Volumetric Cloudscapes of
// Horizon Zero Dawn", 2015) made lumpy, doughy bodies in the towering cumulus prototype (.work/clouds/towers/proto.py).
// The fbm spans only about 0.25 to 0.72, so it is stretched over 0 to 1, or the weather map's dome alone would shape a
// cloud and the billows would show only at its rim; the power rounds the billows' tops.
#include "cloud_noise.hlsli"

RWTexture3D<unorm float> shape : register(u0);

static const float kSize = 128;
static const float kLow = 0.25, kHigh = 0.72;   // its 2nd and 98th percentiles

[numthreads(4, 4, 4)]
void main(uint3 id : SV_DispatchThreadID)
{
    float3 uvw = (id + 0.5) / kSize;
    float worley = worley_fbm(uvw, 4);
    shape[id] = pow(saturate((worley - kLow) / (kHigh - kLow)), 1.5);
}
