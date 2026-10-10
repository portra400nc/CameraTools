// The ambient pass. Writes the whole irradiance target: ours on world pixels, the game's everywhere else, because the
// result is copied over the game's buffer whole.
#include "common.hlsli"

Texture2D<float3> irradiance : register(t0);    // mirror of the game's diffuse irradiance (ambient pair rt1)
Texture2D<float4> normals : register(t1);       // mirror of G-buffer rt0, world normal * 0.5 + 0.5
Texture2D<float> depth : register(t2);          // mirror of the main depth, R32_FLOAT_X8X24_TYPELESS, reversed Z
Texture2D<uint2> stencil : register(t3);        // same mirror, X32_TYPELESS_G8X24_UINT; stencil in .g

static const int kDirections = 8;
static const int kSteps = 4;

// Horizon-style occlusion: how much of the hemisphere around n the depth buffer covers within ao_radius.
float ambient_occlusion(int2 pixel, float3 p, float3 n)
{
    float radius_px = clamp(ao_radius * projection_scale / max(-p.z, 0.05), 1, 64);
    // Interleaved gradient noise rotates the directions per pixel, so 8 directions read as many.
    float noise = frac(52.9829189 * frac(dot(float2(pixel), float2(0.06711056, 0.00583715))));
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

float3 main(float4 position : SV_Position) : SV_Target0
{
    int2 pixel = int2(position.xy);
    float3 game = irradiance.Load(int3(pixel, 0));
    if (!is_world(stencil.Load(int3(pixel, 0)).g))
        return game;

    float3 p = view_position(position.xy * render_size.zw, depth.Load(int3(pixel, 0)));
    float3 n = normalize(mul((float3x3)world_to_view, normals.Load(int3(pixel, 0)).xyz * 2 - 1));
    return game * level * lerp(1, ambient_occlusion(pixel, p, n), ao_strength);
}
