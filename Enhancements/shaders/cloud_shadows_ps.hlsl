// Cloud shadows: lowers the game's sun visibility (the shadow mask) by the light the clouds let through toward the sun,
// at each world pixel, through eight samples of each layer's cloud body between the point and the sun. Writes the whole
// mask: on world pixels .x becomes the game's times the clouds', and .y and every other pixel keep the game's, because the
// result is copied over the game's mask whole. Reads the march's functions and last frame's weather map.
//
// A first draw (cloud_shadow_map.w 2) fills a map of that light over the plane of the lowest layer's base, a texel for
// each point a ray toward the sun enters the clouds; a pixel below the clouds then reads it where its own ray enters, the
// same light for kCloudShadowMapSide squared marches in place of one a pixel. Pixels in or above the clouds, or whose
// ray enters off the map, march their own.
#define CLOUD_LIBRARY
#include "clouds_ps.hlsl"

Texture2D<float2> shadow_mask : register(t0);   // mirror of the game's shadow mask: .x sun visibility, .y untouched
Texture2D<float2> shadow_map : register(t14);   // the first draw's map, while cloud_shadow_map.w is 1

static const uint kShadowSamples = 8;
static const float kCloudShadowMapSide = 384;    // kCloudShadowMapSide in gpu.cpp

// The optical depth through every layer from p toward the sun l.
float shadow_depth(float3 p, float3 l)
{
    float depth = 0;
    [loop] for (uint layer = 0; layer < (cloud_layers > 1.5 ? 2u : 1u); layer++)
    {
        use_layer(layer);
        kind_look = kKinds[min((uint)layer_type, 9u)];
        float t0 = max((layer_base - p.y) / l.y, 0), t1 = (layer_base + layer_thickness - p.y) / l.y;
        if (t1 <= t0)
            continue;
        float dt = (t1 - t0) / kShadowSamples;
        for (uint k = 0; k < kShadowSamples; k++)
            depth += cloud_extinction_coarse(p + l * (t0 + (k + 0.5) * dt)) * dt;
    }
    return depth;
}

float lowest_base()
{
    use_layer(0);
    float base = layer_base;
    if (cloud_layers > 1.5)
    {
        use_layer(1);
        base = min(base, layer_base);
    }
    return base;
}

float2 main(float4 position : SV_Position) : SV_Target0
{
    int2 pixel = int2(position.xy);
    float3 l = cloud_toward_sun;
    skew_direction = float2(cos(radians(cloud_pad)), sin(radians(cloud_pad)));
    if (cloud_shadow_map.w > 1.5)
    {
        if (l.y < 0.02)
            return float2(1, 0);
        float2 xz = cloud_shadow_map.xy + (position.xy / kCloudShadowMapSide - 0.5) * cloud_shadow_map.z;
        return float2(exp(-shadow_depth(float3(xz.x, lowest_base(), xz.y), l)), 0);
    }
    float2 game = shadow_mask.Load(int3(pixel, 0));
    if (!is_world(stencil.Load(int3(pixel, 0)).g) || game.x <= 0)
        return game;
    if (l.y < 0.02)
        return game;
    float3x3 to_world = transpose((float3x3)world_to_view);
    float3 origin = mul(to_world, -float3(world_to_view[0].w, world_to_view[1].w, world_to_view[2].w));
    float3 p = origin + mul(to_world, view_position_at(pixel));
    if (cloud_shadow_map.w > 0.5)
    {
        float base = lowest_base();
        if (p.y < base)
        {
            float2 uv = (p.xz + l.xz * ((base - p.y) / l.y) - cloud_shadow_map.xy) / cloud_shadow_map.z + 0.5;
            if (all(uv > 0) && all(uv < 1))
                return float2(game.x * shadow_map.SampleLevel(linear_clamp, uv, 0).x, game.y);
        }
    }
    return float2(game.x * exp(-shadow_depth(p, l)), game.y);
}
