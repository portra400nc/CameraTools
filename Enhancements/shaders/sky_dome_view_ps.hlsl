// The physical sky drawn in place of the game's sky dome, toward the direction the dome's own vertex shader gives each
// vertex (TEXCOORD8, from the game's camera of this frame), so the sky, the sun and the moon stay put as the camera
// turns. Its inputs repeat the vertex shader's outputs up to TEXCOORD8 in the same registers, as D3D11 links the stages
// (core/dxbc.h, sky_direction_layout). sky_dome_ps.hlsl draws it for a dome shader with other outputs.
#include "common.hlsli"
#include "sky.hlsli"
#include "sky_drawn.hlsli"

struct Dome
{
    float4 position : SV_Position;
    float4 t0 : TEXCOORD0;
    float4 t1 : TEXCOORD1;
    float4 t2 : TEXCOORD2;
    float4 t3 : TEXCOORD3;
    float4 t4 : TEXCOORD4;
    float2 t5 : TEXCOORD5;
    float4 t6 : TEXCOORD6;
    float4 t7 : TEXCOORD7;
    float4 toward : TEXCOORD8;
};

float4 main(Dome dome) : SV_Target0
{
    return float4(drawn_sky_toward(normalize(dome.toward.xyz)), 1);
}
