// The depth of field's post-filter at half size: the gather's fields with their sampling holes filled, and with the bokeh
// contrast on the far field's lifted light given back around it (dof.hlsli).
#include "dof.cbuffer.hlsli"

SamplerState linear_clamp : register(s1);
Texture2D<float4> far_in : register(t0);    // with mips, generated with the bokeh contrast on
Texture2D<float4> near_in : register(t1);
Texture2D<float2> dilated : register(t2);   // the tiles' far and near maxima, dilated

float4 dof_load_far(int2 p) { return far_in.Load(int3(clamp(p, int2(0, 0), int2(dof_sizes.xy) - 1), 0)); }
float4 dof_load_near(int2 p) { return near_in.Load(int3(clamp(p, int2(0, 0), int2(dof_sizes.xy) - 1), 0)); }
float4 dof_sample_far(float2 at, float lod) { return far_in.SampleLevel(linear_clamp, at / float2(dof_sizes.xy), lod); }
float2 dof_load_dilated(int2 t) { return dilated.Load(int3(clamp(t, int2(0, 0), int2(dof_sizes.zw) - 1), 0)); }

#define DOF_POST
#include "dof.hlsli"

struct Fields
{
    float4 far : SV_Target0;
    float4 near : SV_Target1;
};

Fields main(float4 position : SV_Position)
{
    DofFields g = dof_post(int2(position.xy));
    Fields o;
    o.far = g.far;
    o.near = g.near;
    return o;
}
