// The depth of field's half-size source: each pixel the mean of its 2x2 scene pixels, and the green CoC of their nearest
// depth in alpha. Its mips are generated after.
#include "dof.cbuffer.hlsli"

Texture2D<float3> scene : register(t0);     // mirror of the game's HDR scene
Texture2D<float> depth : register(t1);      // mirror of the main depth, reversed Z
Texture2D<float2> focus : register(t2);     // the autofocus's 1x1: the focus and the mean luminance

float3 dof_load_scene(int2 p) { return scene.Load(int3(clamp(p, int2(0, 0), int2(dof_counts.yz) - 1), 0)); }
float dof_load_depth(int2 p) { return depth.Load(int3(clamp(p, int2(0, 0), int2(dof_counts.yz) - 1), 0)); }
float2 dof_load_focus() { return focus.Load(int3(0, 0, 0)); }

#define DOF_PREFILTER
#include "dof.hlsli"

float4 main(float4 position : SV_Position) : SV_Target0
{
    return dof_prefilter(int2(position.xy));
}
