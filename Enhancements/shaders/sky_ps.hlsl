// The physical sky over the sky's pixels (depth 0), sky_drawn.hlsli's, unless sky_dome_ps.hlsl drew it in place of the
// game's dome this frame; and over the scene's pixels the air between them and the viewer. Writes every pixel, because
// the result is copied over the game's HDR scene whole.
#include "common.hlsli"
#include "sky.hlsli"
#include "vegetation.hlsli"
#include "sky_drawn.hlsli"

Texture2D<float3> scene : register(t0);             // mirror of the game's HDR scene
Texture2D<float> depth : register(t2);              // mirror of the main depth, reversed Z
Texture2D<float> aerial_lit : register(t8);         // aerial_lit_ps.hlsl's share of lit air, half size, while sky_aerial_lit
Texture2D<uint> water_marks : register(t15);        // water_ps.hlsl's marks: the frame (top 8 bits) and depth (low 24) of the water

// The reversed depth of the water drawn over the pixel this frame, or 0. The game's water writes no depth, so the haze
// over a sea took the seabed's distance in the shallows and none past the seabed's end, a hard line between the two.
float water_depth_at(int2 pixel)
{
    if (water_stamp == 0)
        return 0;
    uint mark = water_marks.Load(int3(pixel, 0));
    return (mark >> 24) == water_stamp ? float(mark & 0xFFFFFF) / 16777215.0 : 0;
}

// Aerial perspective: the scene point's light through the air between, sky_aerial times as far as it is (sky.hlsli's
// aerial_light).
float3 aerial(float3 color, float meters, float3 view, float lit)
{
    float3 through;
    float3 inscatter = aerial_light(meters * 0.001 * sky_aerial, view, lit, through);
    return color * through + inscatter;
}

float3 main(float4 position : SV_Position) : SV_Target0
{
    int2 pixel = int2(position.xy);
    float3 color = scene.Load(int3(pixel, 0));
    float scene_depth = depth.Load(int3(pixel, 0));
    // Over water, the haze to its surface: nearer than the seabed under it, and over the deep sea, where the depth holds
    // only the sky, there.
    float water_depth = water_depth_at(pixel);
    bool over_water = water_depth > scene_depth;
    if (over_water)
        scene_depth = water_depth;
    if (scene_depth > 0)
    {
        float3 at = view_position(position.xy * render_size.zw, scene_depth);
        if (!over_water)
            color = night_vegetation(color, pixel, at);
        if (sky_aerial <= 0)
            return color;
        float3 toward = normalize(mul(normalize(at), (float3x3)world_to_view));
        float lit = sky_aerial_lit > 0.5 ? aerial_lit.Load(int3(pixel / 2, 0)) : 1;
        return min(aerial(color, length(at), toward, lit), kMostLight);
    }
    // Drawn in place of the game's dome this frame, under what the game drew over it since.
    if (sky_dome > 0.5)
        return color;
    return drawn_sky(position.xy);
}
