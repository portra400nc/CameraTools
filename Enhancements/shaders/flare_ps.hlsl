// The lens flare over the back buffer, after ReShade's effects: its color decoded to linear light, the flare screened over
// it, and encoded back with its own alpha (flare.hlsli).
#include "flare.cbuffer.hlsli"

Texture2D<float4> back : register(t0);          // mirror of the back buffer
Texture2D<float> visibility : register(t1);     // flare_visibility_ps.hlsl's texel

float flare_load_visibility() { return visibility.Load(int3(0, 0, 0)); }

#include "flare.hlsli"

// The sRGB curve an 8-bit or 10-bit back buffer holds, as dof_display.hlsli decodes it.
float to_linear(float v) { return v <= 0.04045 ? v / 12.92 : pow((v + 0.055) / 1.055, 2.4); }
float to_srgb(float c) { return c <= 0.0031308 ? c * 12.92 : 1.055 * pow(c, 1 / 2.4) - 0.055; }

float3 decode(float3 v)
{
    if (flare_frame.z == 0)
        return v;
    v = saturate(v);
    return float3(to_linear(v.r), to_linear(v.g), to_linear(v.b));
}

float3 encode(float3 c)
{
    if (flare_frame.z == 0)
        return c;
    c = saturate(c);
    return float3(to_srgb(c.r), to_srgb(c.g), to_srgb(c.b));
}

float4 main(float4 position : SV_Position) : SV_Target0
{
    float4 b = back.Load(int3(int2(position.xy), 0));
    return float4(encode(flare_over(decode(b.rgb), flare_light(position.xy))), b.a);
}
