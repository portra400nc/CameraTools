// The depth of field's tiles: each 8x8 block of the half-size source's largest far CoC and largest near |CoC|.
#include "dof.cbuffer.hlsli"

Texture2D<float4> source : register(t0);    // the prefilter's, rgb and the green CoC

float4 dof_load_source(int2 p) { return source.Load(int3(clamp(p, int2(0, 0), int2(dof_sizes.xy) - 1), 0)); }

#define DOF_TILES
#include "dof.hlsli"

float2 main(float4 position : SV_Position) : SV_Target0
{
    return dof_tile(int2(position.xy));
}
