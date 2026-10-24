// The depth of field's tiles dilated: each the largest of the tiles a disc from them can reach.
#include "dof.cbuffer.hlsli"

Texture2D<float2> tiles : register(t0);

float2 dof_load_tile(int2 t) { return tiles.Load(int3(clamp(t, int2(0, 0), int2(dof_sizes.zw) - 1), 0)); }

#define DOF_DILATE
#include "dof.hlsli"

float2 main(float4 position : SV_Position) : SV_Target0
{
    return dof_dilate(int2(position.xy));
}
