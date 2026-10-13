// One pass of the gather's edge-aware filter: each world pixel's 5x5 neighbors at gather_apply.y pixels apart, weighted
// as gbuffer.hlsli's blur_weight weighs a tap by depth and normal, averaged into both targets. It smooths the far terms
// only, the bounce, the sky's share, the sky met and the occlusion, before the game multiplies them by the albedo, so no
// texture blurs; the apply draw keeps the pixel's own occlusion unfiltered. Run once at step 1, or twice, the second at
// step 2.
#include "gbuffer.hlsli"
#include "gather.cbuffer.hlsli"

Texture2D<float4> bounce_in : register(t5);     // B, f
Texture2D<float4> sky_in : register(t6);        // K, occlusion

struct Targets
{
    float4 bounce : SV_Target0;
    float4 sky : SV_Target1;
};

Targets main(float4 position : SV_Position)
{
    int2 pixel = int2(position.xy);
    Targets o;
    o.bounce = bounce_in.Load(int3(pixel, 0));
    o.sky = sky_in.Load(int3(pixel, 0));
    if (!is_world(stencil.Load(int3(pixel, 0)).g))
        return o;
    float z = view_position_at(pixel).z;
    float3 n = view_normal_at(pixel);
    int step = int(gather_apply.y);
    float4 bounce = 0, sky = 0;
    float total = 0;
    [loop] for (int y = -2; y <= 2; y++)
    {
        [unroll] for (int x = -2; x <= 2; x++)
        {
            int2 q = pixel + int2(x, y) * step;
            float weight = blur_weight(q, z, n);
            if (weight <= 0)
                continue;
            bounce += bounce_in.Load(int3(q, 0)) * weight;
            sky += sky_in.Load(int3(q, 0)) * weight;
            total += weight;
        }
    }
    // The center is world and counts with weight 1.
    o.bounce = bounce / total;
    o.sky = sky / total;
    return o;
}
