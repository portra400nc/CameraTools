// The ambient pass's third draw: the game's reflections (ambient pair rt0) on world pixels, occluded as the ambient light
// is, since a probe's light reaches a crevice no more than the sky's does; with the physical sky, at its share of a day's
// light, which arrives in level, and grayed by the overcast, which the probes do not see: they see the drawn sky, which our
// clouds cover only after. The game's everywhere else. The game keeps its reflections as bright at night as by day.
#include "gbuffer.hlsli"

Texture2D<float4> reflections : register(t0);   // mirror of the game's ambient specular

float4 main(float4 position : SV_Position) : SV_Target0
{
    int2 pixel = int2(position.xy);
    float4 game = reflections.Load(int3(pixel, 0));
    uint s = stencil.Load(int3(pixel, 0)).g;
    if (!is_world(s))
        return game;
    float3 gray = dot(game.rgb, float3(0.2126, 0.7152, 0.0722));
    float strength = ao_strength * (is_foliage(s) ? foliage_ao_strength : 1);
    float occlusion = strength > 0 ? blurred(pixel, view_position_at(pixel).z, view_normal_at(pixel)) : 1;
    return float4(lerp(game.rgb, gray, reflection_overcast) * level * lerp(1, occlusion, strength), game.a);
}
