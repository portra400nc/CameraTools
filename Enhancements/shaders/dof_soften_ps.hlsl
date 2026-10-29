// The lens softness at full size, first in the depth of field's chain, or alone while the depth of field is off.
#include "dof.cbuffer.hlsli"

Texture2D<float3> scene : register(t0);     // the HDR scene's mirror, or dof_display_in_ps.hlsl's linear color

float3 dof_load_scene(int2 p) { return scene.Load(int3(clamp(p, int2(0, 0), int2(dof_counts.yz) - 1), 0)); }

#define DOF_SOFTEN
#include "dof.hlsli"

float3 main(float4 position : SV_Position) : SV_Target0
{
    return dof_soften(int2(position.xy));
}
