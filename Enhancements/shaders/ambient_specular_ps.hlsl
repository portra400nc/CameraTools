// The ambient pass's third draw, with the physical sky: the game's reflections (ambient pair rt0) on world pixels at the
// physical sky's share of a day's light, which arrives in level; the game's everywhere else. The game keeps its
// reflections as bright at night as by day.
#include "gbuffer.hlsli"

Texture2D<float4> reflections : register(t0);   // mirror of the game's ambient specular

float4 main(float4 position : SV_Position) : SV_Target0
{
    int2 pixel = int2(position.xy);
    float4 game = reflections.Load(int3(pixel, 0));
    return is_world(stencil.Load(int3(pixel, 0)).g) ? float4(game.rgb * level, game.a) : game;
}
