// The depth of field's composite at full size, written through a mirror of the HDR scene.
#include "dof.cbuffer.hlsli"

Texture2D<float3> scene : register(t0);     // mirror of the game's HDR scene
Texture2D<float> depth : register(t1);      // mirror of the main depth, reversed Z
Texture2D<float2> focus : register(t2);     // the autofocus's 1x1: the focus and the mean luminance
Texture2D<float4> far_in : register(t3);    // the post-filter's fields
Texture2D<float4> near_in : register(t4);

float3 dof_load_scene(int2 p) { return scene.Load(int3(clamp(p, int2(0, 0), int2(dof_counts.yz) - 1), 0)); }
float dof_load_depth(int2 p) { return depth.Load(int3(clamp(p, int2(0, 0), int2(dof_counts.yz) - 1), 0)); }
float2 dof_load_focus() { return focus.Load(int3(0, 0, 0)); }
float4 dof_load_far(int2 p) { return far_in.Load(int3(clamp(p, int2(0, 0), int2(dof_sizes.xy) - 1), 0)); }
float4 dof_load_near(int2 p) { return near_in.Load(int3(clamp(p, int2(0, 0), int2(dof_sizes.xy) - 1), 0)); }

#define DOF_COMPOSITE
#include "dof.hlsli"

float3 main(float4 position : SV_Position) : SV_Target0
{
    return dof_composite(int2(position.xy));
}
