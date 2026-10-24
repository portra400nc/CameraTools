// The depth of field's tiles: each 8x8 block of the half-size source's widest disc behind and in front of focus.
#include "dof.cbuffer.hlsli"

Texture2D<float4> source : register(t0);    // the prefilter's, rgb and the green CoC
Texture2D<float2> focus : register(t1);     // the autofocus's 1x1: the focus and the mean luminance

float4 dof_load_source(int2 p) { return source.Load(int3(clamp(p, int2(0, 0), int2(dof_sizes.xy) - 1), 0)); }
float2 dof_load_focus() { return focus.Load(int3(0, 0, 0)); }

#define DOF_TILES
#include "dof.hlsli"

float2 main(float4 position : SV_Position) : SV_Target0
{
    return dof_tile(int2(position.xy));
}
