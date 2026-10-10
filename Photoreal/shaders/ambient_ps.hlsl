// The ambient pass's second draw: blurs ao_ps.hlsl's raw occlusion and applies it. Writes the whole irradiance target:
// ours on world pixels, the game's everywhere else, because the result is copied over the game's buffer whole.
#include "ambient.hlsli"

Texture2D<float> raw_ao : register(t4);         // ao_ps.hlsl's output

// A tap's weight falls to 0 at this view-depth difference relative to the center's depth. Ground seen at 5 degrees
// differs about 4% over two pixels at 720p and keeps most of its weight; a branch in front of a wall drops out.
static const float kDepthTolerance = 0.1;

// Grass, vegetation and foliage: alpha-tested, so their depth is noisy at the pixel scale and so is their occlusion.
bool is_foliage(uint s) { return s == 129 || s == 136 || s == 137; }

// A 4x4 window, one whole period of ao_ps.hlsl's pattern, so a flat surface averages its 16 rotations exactly.
// Taps that are not world (sky, characters) are skipped, and taps across a depth jump or a sharp normal change fade out.
float blurred_occlusion(int2 pixel, float z, float3 n)
{
    float sum = 0, total = 0;
    [unroll] for (int y = -2; y < 2; y++)
    {
        [unroll] for (int x = -2; x < 2; x++)
        {
            int2 q = pixel + int2(x, y);
            if (any(q < 0) || any(q >= int2(render_size.xy)) || !is_world(stencil.Load(int3(q, 0)).g))
                continue;
            float depth_weight = saturate(1 - abs(view_position_at(q).z - z) / (max(-z, 0.05) * kDepthTolerance));
            float facing = dot(n, view_normal_at(q)) * 0.5 + 0.5;
            float weight = depth_weight * facing * facing;
            sum += raw_ao.Load(int3(q, 0)) * weight;
            total += weight;
        }
    }
    return sum / total;     // the center is world and counts with weight 1
}

float3 main(float4 position : SV_Position) : SV_Target0
{
    int2 pixel = int2(position.xy);
    float3 game = irradiance.Load(int3(pixel, 0));
    uint s = stencil.Load(int3(pixel, 0)).g;
    if (!is_world(s))
        return game;

    float strength = ao_strength * (is_foliage(s) ? foliage_ao_strength : 1);
    float occlusion = blurred_occlusion(pixel, view_position_at(pixel).z, view_normal_at(pixel));
    return game * level * lerp(1, occlusion, strength);
}
