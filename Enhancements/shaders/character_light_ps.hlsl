// The character light pass's second draw, at the bloom step before the traced lights: CameraTools' lights light the
// characters as studio lamps. A character pixel keeps params.x of the game's light and takes each light's albedo-weighted
// key, a toon step or a soft falloff across its normal, behind character_shadow_ps.hlsl's visibility averaged over the 4x4
// window, and a rim where the light stands behind it. Writes the whole target, because the result is copied over the
// game's HDR scene whole.
#include "character_light.hlsli"

Texture2D<float3> scene : register(t0);     // mirror of the game's HDR scene
Texture2D<float4> shadow : register(t7);    // the first draw's raw visibility, a light a channel

float3 srgb_to_linear(float3 c) { return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4); }

// The raw visibility over one whole period of pattern(), so a smooth surface averages its 16 offsets. Taps off the
// character, and across a depth jump or a sharp normal change, fade out, as gbuffer.hlsli's blurred() weighs them.
float4 shadow_at(int2 pixel, float z, float3 n)
{
    float4 sum = 0;
    float total = 0;
    [unroll] for (int y = -2; y < 2; y++)
    {
        [unroll] for (int x = -2; x < 2; x++)
        {
            int2 q = pixel + int2(x, y);
            if (any(q < 0) || any(q >= int2(render_size.xy)) || !is_character(stencil.Load(int3(q, 0)).g))
                continue;
            float depth_weight = saturate(1 - abs(view_position_at(q).z - z) / (max(-z, 0.05) * kDepthTolerance));
            float facing = dot(n, view_normal_at(q)) * 0.5 + 0.5;
            float weight = depth_weight * facing * facing;
            sum += shadow.Load(int3(q, 0)) * weight;
            total += weight;
        }
    }
    return total > 0 ? sum / total : 1;
}

float3 main(float4 position : SV_Position) : SV_Target0
{
    int2 pixel = int2(position.xy);
    float3 color = scene.Load(int3(pixel, 0));
    if (depth.Load(int3(pixel, 0)) <= 0 || !is_character(stencil.Load(int3(pixel, 0)).g))
        return color;
    float3 p = view_position_at(pixel);
    bool face = is_face(pixel);
    float3 n = character_normal(pixel, p, face);
    float3 v = normalize(-p);
    float4 seen = shadow_at(pixel, p.z, view_normal_at(pixel));
    float3 base = srgb_to_linear(albedo.Load(int3(pixel, 0)).rgb);
    float facing = 1 - saturate(dot(n, v));
    float fresnel = facing * sqrt(facing);
    float2 edge = n.xy / max(length(n.xy), 1e-4);
    float3 key = 0, rim = 0;
    [unroll] for (uint k = 0; k < 4; k++)
    {
        if (k >= light_info.x)
            break;
        float4 r = local_reach_of(k, p);
        if (r.w <= 0)
            continue;
        float3 l = r.xyz;
        float3 radiance = light_rows[k * 4u + 1u].xyz * r.w;
        float nl = dot(n, l);
        float shade = params.z > 0.5 ? pow(saturate((nl + 0.2) / 1.2), 1.5) : smoothstep(0.55, 0.65, nl * 0.5 + 0.5);
        key += radiance * shade * (face ? max(seen[k], 0.5) : seen[k]);
        // Edges that turn away from the camera glow when the light stands behind them, on the light's side of the silhouette.
        float behind = saturate(0.5 - 0.5 * dot(v, l));
        float side = length(l.xy) > 0.2 ? saturate(dot(edge, normalize(l.xy)) * 1.5 + 0.25) : 1;
        rim += radiance * fresnel * behind * side;
    }
    return color * params.x + base * key + params.y * rim * (0.25 + base);
}
