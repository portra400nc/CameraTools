// Cloud shadows: lowers the game's sun visibility (the shadow mask) by the light the clouds let through toward the sun,
// at each world pixel, through eight samples of each layer's cloud body between the point and the sun. Writes the whole
// mask: on world pixels .x becomes the game's times the clouds', and .y and every other pixel keep the game's, because the
// result is copied over the game's mask whole. Reads the march's functions and last frame's weather map.
#define CLOUD_LIBRARY
#include "clouds_ps.hlsl"

Texture2D<float2> shadow_mask : register(t0);   // mirror of the game's shadow mask: .x sun visibility, .y untouched

static const uint kShadowSamples = 8;

float2 main(float4 position : SV_Position) : SV_Target0
{
    int2 pixel = int2(position.xy);
    float2 game = shadow_mask.Load(int3(pixel, 0));
    if (!is_world(stencil.Load(int3(pixel, 0)).g) || game.x <= 0)
        return game;
    float3 l = cloud_toward_sun;
    if (l.y < 0.02)
        return game;
    float3x3 to_world = transpose((float3x3)world_to_view);
    float3 origin = mul(to_world, -float3(world_to_view[0].w, world_to_view[1].w, world_to_view[2].w));
    float3 p = origin + mul(to_world, view_position_at(pixel));
    skew_direction = float2(cos(radians(cloud_pad)), sin(radians(cloud_pad)));
    float depth = 0;
    [loop] for (uint layer = 0; layer < (cloud_layers > 1.5 ? 2u : 1u); layer++)
    {
        use_layer(layer);
        kind_look = kKinds[min((uint)layer_type, 4u)];
        float t0 = max((layer_base - p.y) / l.y, 0), t1 = (layer_base + layer_thickness - p.y) / l.y;
        if (t1 <= t0)
            continue;
        float dt = (t1 - t0) / kShadowSamples;
        for (uint k = 0; k < kShadowSamples; k++)
            depth += cloud_extinction_coarse(p + l * (t0 + (k + 0.5) * dt)) * dt;
    }
    return float2(game.x * exp(-depth), game.y);
}
