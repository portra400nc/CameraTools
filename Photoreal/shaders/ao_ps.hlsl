// The ambient pass's first draw: raw occlusion into our render-size scratch, 1 on pixels that are not world. Each pixel
// rotates its directions by a 4x4 pattern, so the raw result is noisy; ambient_ps.hlsl's 4x4 blur averages it away.
#include "ambient.hlsli"

static const int kDirections = 8;
static const int kSteps = 4;

// The 4x4 Bayer order, (0 8 2 10 / 12 4 14 6 / 3 11 1 9 / 15 7 13 5) / 16. Any 4x4 window of pixels holds each of the
// 16 values once, so one blur window sees 16 rotations of the 8 directions, evenly spread.
float pattern(int2 pixel)
{
    uint2 p = uint2(pixel) & 3u;
    uint z = p.x ^ p.y;
    return ((z & 1u) * 8 + (p.y & 1u) * 4 + (z >> 1) * 2 + (p.y >> 1) + 0.5) / 16;
}

// Horizon-style occlusion: how much of the hemisphere around n the depth buffer covers within ao_radius.
float ambient_occlusion(int2 pixel, float3 p, float3 n)
{
    float radius_px = clamp(ao_radius * projection_scale / max(-p.z, 0.05), 1, 64);
    float noise = pattern(pixel);
    float occlusion = 0;
    [loop] for (int d = 0; d < kDirections; d++)
    {
        float angle = (d + noise) * (6.2831853 / kDirections);
        float2 direction = float2(cos(angle), sin(angle));
        [unroll] for (int s = 0; s < kSteps; s++)
        {
            int2 q = pixel + int2(round(direction * radius_px * (s + 0.5 + 0.5 * noise) / kSteps));
            if (any(q < 0) || any(q >= int2(render_size.xy)))
                continue;
            float d_q = depth.Load(int3(q, 0));
            if (d_q <= 0)
                continue;
            float3 v = view_position((q + 0.5) * render_size.zw, d_q) - p;
            float distance2 = dot(v, v);
            float cosine = dot(v, n) * rsqrt(max(distance2, 1e-8));
            occlusion += saturate(cosine - 0.1) * saturate(1 - distance2 / (ao_radius * ao_radius));
        }
    }
    return saturate(1 - 2 * occlusion / (kDirections * kSteps));
}

float main(float4 position : SV_Position) : SV_Target0
{
    int2 pixel = int2(position.xy);
    if (!is_world(stencil.Load(int3(pixel, 0)).g))
        return 1;
    return ambient_occlusion(pixel, view_position_at(pixel), view_normal_at(pixel));
}
