// The physical sky drawn in place of the game's sky dome, with the dome's own geometry and depth and stencil tests, so
// what the game draws over its sky after it, such as the giant tree in Snezhnaya's sky, lands on ours.
#include "common.hlsli"
#include "sky.hlsli"
#include "sky_drawn.hlsli"

float4 main(float4 position : SV_Position) : SV_Target0
{
    return float4(drawn_sky(position.xy), 1);
}
