// The game's sky dome drawn again with no depth test into a target of ours, which then holds the direction from this
// frame's camera at nearly every pixel: sky_camera_cs.hlsl finds the camera's turn from it.
#include "sky_dome_inputs.hlsli"

float4 main(Dome dome) : SV_Target0
{
    return float4(normalize(dome.toward.xyz), 1);
}
