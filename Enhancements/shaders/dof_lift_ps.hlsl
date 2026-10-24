// The depth of field's lifted source at half size, read by the gather's far field with the bokeh contrast on: each source's
// light times its lift, and its own luminance (dof.hlsli). Its mips are generated after.
#include "dof.cbuffer.hlsli"

SamplerState linear_clamp : register(s1);
Texture2D<float4> source : register(t0);    // the prefilter's, rgb and the green CoC, with mips

float4 dof_load_source(int2 p) { return source.Load(int3(clamp(p, int2(0, 0), int2(dof_sizes.xy) - 1), 0)); }
float3 dof_sample_source(float2 at, float lod) { return source.SampleLevel(linear_clamp, at / float2(dof_sizes.xy), lod).rgb; }

#define DOF_LIFT
#include "dof.hlsli"

float4 main(float4 position : SV_Position) : SV_Target0
{
    return dof_lift(int2(position.xy));
}
