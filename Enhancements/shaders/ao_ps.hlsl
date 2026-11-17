// The ambient pass's first draw: raw occlusion into our render-size scratch, 1 on pixels that are not world. Each pixel
// rotates its directions by a 4x4 pattern, so the raw result is noisy; ambient_ps.hlsl's 4x4 blur averages it away.
#include "gbuffer.hlsli"

static const int kDirections = 8;
// The reach is held to a twelfth of the picture's height, 64 pixels at 800, so it covers the same share of a 4K shot as of
// the live view; a longer reach takes more steps, up to 10, so its samples stay a dozen pixels or so apart.
static const float kReachShare = 64.0 / 800;

// Horizon-style occlusion: how much of the hemisphere around n the depth buffer covers within ao_radius.
float ambient_occlusion(int2 pixel, float3 p, float3 n)
{
    float radius_px = clamp(ao_radius * projection_scale / max(-p.z, 0.05), 1, kReachShare * render_size.y);
    int steps = clamp(int(radius_px / 12), 4, 10);
    float noise = pattern(pixel);
    float occlusion = 0;
    [loop] for (int d = 0; d < kDirections; d++)
    {
        float angle = (d + noise) * (6.2831853 / kDirections);
        float2 direction = float2(cos(angle), sin(angle));
        [loop] for (int s = 0; s < steps; s++)
        {
            int2 q = pixel + int2(round(direction * radius_px * (s + 0.5 + 0.5 * noise) / steps));
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
    return saturate(1 - 2 * occlusion / (kDirections * steps));
}

float main(float4 position : SV_Position) : SV_Target0
{
    int2 pixel = int2(position.xy);
    if (!is_world(stencil.Load(int3(pixel, 0)).g))
        return 1;
    return ambient_occlusion(pixel, view_position_at(pixel), view_normal_at(pixel));
}
