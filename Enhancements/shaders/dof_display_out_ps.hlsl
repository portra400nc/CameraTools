// The depth of field on the display, last: the composite encoded for the back buffer, with its own alpha
// (dof_display.hlsli).
#include "dof.cbuffer.hlsli"

Texture2D<float3> blurred : register(t0);   // the composite
Texture2D<float4> display : register(t1);   // dof_display_in_ps.hlsl's color

float3 dof_load_blurred(int2 p) { return blurred.Load(int3(clamp(p, int2(0, 0), int2(dof_counts.yz) - 1), 0)); }
float4 dof_load_display(int2 p) { return display.Load(int3(clamp(p, int2(0, 0), int2(dof_counts.yz) - 1), 0)); }

#define DOF_DISPLAY_OUT
#include "dof_display.hlsli"

float4 main(float4 position : SV_Position) : SV_Target0
{
    return dof_display_out(int2(position.xy));
}
