// The depth of field's depth bands continued behind the nearer ones at half size (dof.hlsli), as dof_near_mask_ps lays
// them out. Their mips follow.
#include "dof.cbuffer.hlsli"

SamplerState linear_clamp : register(s1);
Texture2D<float2> focus : register(t3);         // the autofocus's 1x1: the focus and the mean luminance
Texture2DArray<float4> bands : register(t4);    // dof_near_mask_ps's, with mips

float2 dof_load_focus() { return focus.Load(int3(0, 0, 0)); }
float4 dof_sample_near_band(int band, float2 at, float lod) { return bands.SampleLevel(linear_clamp, float3(at / float2(dof_sizes.xy), float(band)), lod); }
float4 dof_sample_near_radii(float2 at, float lod) { return bands.SampleLevel(linear_clamp, float3(at / float2(dof_sizes.xy), 4.0f), lod); }
float4 dof_sample_near_split(float2 at, float lod) { return bands.SampleLevel(linear_clamp, float3(at / float2(dof_sizes.xy), 5.0f), lod); }

#define DOF_NEAR_FILL
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
    DofNearBands n = dof_near_fill(int2(position.xy));
    Bands o;
    o.band0 = n.band0;
    o.band1 = n.band1;
    o.band2 = n.band2;
    o.band3 = n.band3;
    o.radii = n.radii;
    o.split = n.split;
    return o;
}
