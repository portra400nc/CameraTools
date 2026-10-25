// The depth of field's near mask at half size: 1 where the source lies in front of focus (dof.hlsli). Its mips follow.
#include "dof.cbuffer.hlsli"

Texture2D<float4> source : register(t0);    // the prefilter's, rgb and the green CoC

float4 dof_load_source(int2 p) { return source.Load(int3(clamp(p, int2(0, 0), int2(dof_sizes.xy) - 1), 0)); }

#define DOF_NEAR_MASK
#include "dof.hlsli"

float4 main(float4 position : SV_Position) : SV_Target0
{
    return dof_near_mask(int2(position.xy));
}
