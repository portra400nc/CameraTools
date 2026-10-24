// The depth of field's gather at half size: the far field, normalized, and the near field, premultiplied (dof.hlsli).
#include "dof.cbuffer.hlsli"

SamplerState linear_clamp : register(s1);
Texture2D<float4> source : register(t0);    // the prefilter's, rgb and the green CoC, with mips
Texture2D<float2> dilated : register(t1);   // the tiles' far and near maxima, dilated
Texture2D<float2> focus : register(t2);     // the autofocus's 1x1: the focus and the mean luminance

float4 dof_load_source(int2 p) { return source.Load(int3(clamp(p, int2(0, 0), int2(dof_sizes.xy) - 1), 0)); }
float3 dof_sample_source(float2 at, float lod) { return source.SampleLevel(linear_clamp, at / float2(dof_sizes.xy), lod).rgb; }
float2 dof_load_dilated(int2 t) { return dilated.Load(int3(clamp(t, int2(0, 0), int2(dof_sizes.zw) - 1), 0)); }
float2 dof_load_focus() { return focus.Load(int3(0, 0, 0)); }

#define DOF_GATHER
#include "dof.hlsli"

struct Fields
{
    float4 far : SV_Target0;
    float4 near : SV_Target1;
};

Fields main(float4 position : SV_Position)
{
    DofFields g = dof_gather(int2(position.xy));
    Fields o;
    o.far = g.far;
    o.near = g.near;
    return o;
}
