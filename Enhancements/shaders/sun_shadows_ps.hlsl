// The sun-shadow pass's second draw: blurs sun_ps.hlsl's raw visibility and writes it as the shadow mask's .x on world
// pixels in place of the game's, so the wider penumbra shows. .y and every other pixel keep the game's value, because
// the result is copied over the game's mask whole.
#include "gbuffer.hlsli"

Texture2D<float2> shadow_mask : register(t0);   // mirror of the game's shadow mask

float2 main(float4 position : SV_Position) : SV_Target0
{
    int2 pixel = int2(position.xy);
    float2 game = shadow_mask.Load(int3(pixel, 0));
    if (!is_world(stencil.Load(int3(pixel, 0)).g))
        return game;
    return float2(blurred(pixel, view_position_at(pixel).z, view_normal_at(pixel)), game.y);
}
