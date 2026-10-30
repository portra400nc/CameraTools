// The depth of field's near plane at half size, after Cinematic DOF's (dof.hlsli): rgb premultiplied and its alpha.
#include "dof.cbuffer.hlsli"

SamplerState linear_clamp : register(s1);
Texture2D<float4> source : register(t0);    // the prefilter's, rgb and the green CoC, with mips
Texture2D<float4> far_in : register(t1);    // the post-filter's far field, with the far sprites
Texture2D<float2> dilated : register(t2);   // the tiles' far and near maxima, dilated
Texture2D<float2> focus : register(t3);     // the autofocus's 1x1: the focus and the mean luminance
Texture2DArray<float4> bands : register(t4);    // dof_near_fill_ps's depth bands, with mips

float4 dof_load_source(int2 p) { return source.Load(int3(clamp(p, int2(0, 0), int2(dof_sizes.xy) - 1), 0)); }
float4 dof_sample_source(float2 at, float lod) { return source.SampleLevel(linear_clamp, at / float2(dof_sizes.xy), lod); }
float4 dof_load_far(int2 p) { return far_in.Load(int3(clamp(p, int2(0, 0), int2(dof_sizes.xy) - 1), 0)); }
float2 dof_load_dilated(int2 t) { return dilated.Load(int3(clamp(t, int2(0, 0), int2(dof_sizes.zw) - 1), 0)); }
float2 dof_load_focus() { return focus.Load(int3(0, 0, 0)); }
float4 dof_sample_near_band(int band, float2 at, float lod) { return bands.SampleLevel(linear_clamp, float3(at / float2(dof_sizes.xy), float(band)), lod); }
float4 dof_sample_near_radii(float2 at, float lod) { return bands.SampleLevel(linear_clamp, float3(at / float2(dof_sizes.xy), 4.0f), lod); }
float4 dof_sample_near_split(float2 at, float lod) { return bands.SampleLevel(linear_clamp, float3(at / float2(dof_sizes.xy), 5.0f), lod); }

#define DOF_NEAR
#include "dof.hlsli"

struct Near
{
    float4 layer : SV_Target0;
    float2 cover : SV_Target1;
};

Near main(float4 position : SV_Position)
{
    DofNear n = dof_near_layer(int2(position.xy));
    Near o;
    o.layer = n.layer;
    o.cover = n.cover;
    return o;
}
