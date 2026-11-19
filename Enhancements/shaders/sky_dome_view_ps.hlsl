// The physical sky drawn in place of the game's sky dome, toward the direction the dome's own vertex shader gives each
// vertex (TEXCOORD8, from the game's camera of this frame), so the sky, the sun and the moon stay put as the camera
// turns. Its inputs repeat the vertex shader's outputs up to TEXCOORD8 in the same registers, as D3D11 links the stages
// (core/dxbc.h, sky_direction_layout). sky_dome_ps.hlsl draws it for a dome shader with other outputs.
#include "common.hlsli"
#include "sky.hlsli"
#include "sky_drawn.hlsli"
#include "sky_dome_inputs.hlsli"

float4 main(Dome dome) : SV_Target0
{
    return float4(drawn_sky_toward(normalize(dome.toward.xyz)), 1);
}
