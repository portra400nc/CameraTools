// The depth of field's autofocus: a 1x1 target that holds the focus distance in meters, eased from the last frame's
// toward the setting's or the nearest depth around the focus point, and the frame's mean luminance for the highlights.
#include "dof.cbuffer.hlsli"

SamplerState linear_clamp : register(s1);
Texture2D<float> depth : register(t0);      // mirror of the main depth, reversed Z
Texture2D<float2> previous : register(t1);  // the last frame's
Texture2D<float3> scene : register(t2);     // mirror of the game's HDR scene

float dof_load_depth(int2 p) { return depth.Load(int3(clamp(p, int2(0, 0), int2(dof_counts.yz) - 1), 0)); }
float3 dof_sample_scene(float2 at) { return scene.SampleLevel(linear_clamp, at / float2(dof_counts.yz), 0.0f); }

#define DOF_FOCUS
#include "dof.hlsli"

float2 main(float4 position : SV_Position) : SV_Target0
{
    return dof_focus_pass(previous.Load(int3(0, 0, 0)).x);
}
