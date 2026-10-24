// The depth of field's highlight sprites at half size, added into the post-filter's far or near field (dof.hlsli).
#include "dof.cbuffer.hlsli"

Texture2D<float4> source : register(t1);    // the gather's source: rgb and the green CoC
Texture2D<float2> focus : register(t2);     // the autofocus's 1x1: the focus and the mean luminance

float4 dof_load_source(int2 p) { return source.Load(int3(clamp(p, int2(0, 0), int2(dof_sizes.xy) - 1), 0)); }
float2 dof_load_focus() { return focus.Load(int3(0, 0, 0)); }

#define DOF_SPRITE
#include "dof.hlsli"

float4 main(float4 position : SV_Position, nointerpolation float4 at : TEXCOORD0, nointerpolation float4 light : TEXCOORD1) : SV_Target0
{
    DofSprite s;
    s.at = at;
    s.light = light;
    float3 c = dof_sprite_light(s, int2(position.xy));
    return float4(c.x, c.y, c.z, 0.0f);
}
