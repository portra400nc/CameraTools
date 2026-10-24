// The depth of field's autofocus: a 1x1 target that holds the focus distance in meters, the setting's, or eased from
// the last frame's toward the nearest depth around the focus point.
#include "dof.cbuffer.hlsli"

Texture2D<float> depth : register(t0);      // mirror of the main depth, reversed Z
Texture2D<float> previous : register(t1);   // the last frame's focus

float dof_load_depth(int2 p) { return depth.Load(int3(clamp(p, int2(0, 0), int2(dof_counts.yz) - 1), 0)); }

#define DOF_FOCUS
#include "dof.hlsli"

float main(float4 position : SV_Position) : SV_Target0
{
    return dof_focus_next(previous.Load(int3(0, 0, 0)));
}
