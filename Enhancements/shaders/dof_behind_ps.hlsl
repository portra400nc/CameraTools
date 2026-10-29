// The depth of field's background at half size, read by the gather's fill behind a blurred foreground: each source behind
// focus with weight 1, the rest with none (dof.hlsli). Its mips are generated after.
#include "dof.cbuffer.hlsli"

Texture2D<float4> source : register(t0);    // the prefilter's, rgb and the green CoC

float4 dof_load_source(int2 p) { return source.Load(int3(clamp(p, int2(0, 0), int2(dof_sizes.xy) - 1), 0)); }

#define DOF_BEHIND
#include "dof.hlsli"

float4 main(float4 position : SV_Position) : SV_Target0
{
    return dof_behind(int2(position.xy));
}
