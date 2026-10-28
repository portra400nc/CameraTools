// The ambient pass's second draw: blurs ao_ps.hlsl's raw occlusion and applies it. Writes the whole irradiance target:
// ours on world pixels, the game's everywhere else, because the result is copied over the game's buffer whole.
#include "gbuffer.hlsli"

Texture2D<float3> irradiance : register(t0);    // mirror of the game's diffuse irradiance (ambient pair rt1)

float3 main(float4 position : SV_Position) : SV_Target0
{
    int2 pixel = int2(position.xy);
    float3 game = irradiance.Load(int3(pixel, 0));
    uint s = stencil.Load(int3(pixel, 0)).g;
    if (!is_world(s))
        return game;

    float strength = ao_strength * (is_foliage(s) ? foliage_ao_strength : 1);
    float occlusion = blurred(pixel, view_position_at(pixel).z, view_normal_at(pixel));
    float3 light = lerp(game, dot(game, float3(0.2126, 0.7152, 0.0722)), ambient_neutral);
    return light * level * lerp(1, occlusion, strength);
}
