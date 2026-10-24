// The depth of field's post-filter at half size: the gather's fields with their sampling holes filled by a 3x3 median.
#include "dof.cbuffer.hlsli"

Texture2D<float4> far_in : register(t0);
Texture2D<float4> near_in : register(t1);

float4 dof_load_far(int2 p) { return far_in.Load(int3(clamp(p, int2(0, 0), int2(dof_sizes.xy) - 1), 0)); }
float4 dof_load_near(int2 p) { return near_in.Load(int3(clamp(p, int2(0, 0), int2(dof_sizes.xy) - 1), 0)); }

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
