// The contact-shadow pass's second draw: blurs contact_ps.hlsl's raw sun visibility and lowers the game's with it. Writes
// the whole shadow mask: on world pixels .x becomes the lower of the game's and ours, and .y and every other pixel keep
// the game's value, because the result is copied over the game's mask whole.
#include "gbuffer.hlsli"

Texture2D<float2> shadow_mask : register(t0);   // mirror of the game's shadow mask: .x sun visibility, .y untouched

float2 main(float4 position : SV_Position) : SV_Target0
{
    int2 pixel = int2(position.xy);
    float2 game = shadow_mask.Load(int3(pixel, 0));
    uint s = stencil.Load(int3(pixel, 0)).g;
    if (!is_world(s))
        return game;
    float strength = contact_strength * (is_foliage(s) ? contact_foliage_strength : 1);
    float visibility = lerp(1, blurred(pixel, view_position_at(pixel).z, view_normal_at(pixel)), strength);
    return float2(min(game.x, visibility), game.y);
}
