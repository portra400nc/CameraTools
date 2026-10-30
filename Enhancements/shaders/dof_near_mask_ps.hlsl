// The depth of field's depth bands in front of focus at half size (dof.hlsli), one layer each of an array, then their
// radii and the splits between them. Their mips follow.
#include "dof.cbuffer.hlsli"

Texture2D<float4> source : register(t0);    // the prefilter's, rgb and the green CoC

float4 dof_load_source(int2 p) { return source.Load(int3(clamp(p, int2(0, 0), int2(dof_sizes.xy) - 1), 0)); }

#define DOF_NEAR_MASK
#include "dof.hlsli"

struct Bands
{
    float4 band0 : SV_Target0;
    float4 band1 : SV_Target1;
    float4 band2 : SV_Target2;
    float4 band3 : SV_Target3;
    float4 radii : SV_Target4;
    float4 split : SV_Target5;
};

Bands main(float4 position : SV_Position)
{
    DofNearBands n = dof_near_bands(int2(position.xy));
    Bands o;
    o.band0 = n.band0;
    o.band1 = n.band1;
    o.band2 = n.band2;
    o.band3 = n.band3;
    o.radii = n.radii;
    o.split = n.split;
    return o;
}
