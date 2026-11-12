// The ambient pass's second draw: blurs ao_ps.hlsl's raw occlusion and applies it. Writes the whole irradiance target:
// ours on world pixels, the game's everywhere else, because the result is copied over the game's buffer whole.
#include "gbuffer.hlsli"

Texture2D<float3> irradiance : register(t0);    // mirror of the game's diffuse irradiance (ambient pair rt1)

// The physical sky's harmonics (core/light.h's SkyMap::harmonics), or unbound and zero while the game's sky lights the world.
cbuffer SkyColor : register(b2)
{
    float4 physical[9];
};

static const float3 kLuma = float3(0.2126, 0.7152, 0.0722);

// The physical sky's light on a unit world normal, as core/light.h's sky_irradiance gives it.
float3 physical_light(float3 n)
{
    return physical[0].rgb + physical[1].rgb * n.y + physical[2].rgb * n.z + physical[3].rgb * n.x + physical[4].rgb * (n.x * n.y)
        + physical[5].rgb * (n.y * n.z) + physical[6].rgb * (3 * n.z * n.z - 1) + physical[7].rgb * (n.x * n.z) + physical[8].rgb * (n.x * n.x - n.y * n.y);
}

float3 main(float4 position : SV_Position) : SV_Target0
{
    int2 pixel = int2(position.xy);
    float3 game = irradiance.Load(int3(pixel, 0));
    uint s = stencil.Load(int3(pixel, 0)).g;
    if (!is_world(s))
        return game;

    float strength = ao_strength * (is_foliage(s) ? foliage_ao_strength : 1);
    float occlusion = blurred(pixel, view_position_at(pixel).z, view_normal_at(pixel));
    float3 light = lerp(game, dot(game, kLuma), ambient_neutral);
    float3 sky = physical_light(normalize(normals.Load(int3(pixel, 0)).xyz * 2 - 1));
    if (dot(sky, kLuma) > 1e-6)
        light = dot(game, kLuma) * sky / dot(sky, kLuma);
    return light * level * lerp(1, occlusion, strength);
}
