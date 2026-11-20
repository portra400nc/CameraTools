// The leaves' copy at the combine step, after our passes there: each grass and leaf pixel's albedo as stored and the sun's
// visibility in the shadow mask, which the game's shadow map and our shadow passes lowered, its world normal with a
// mark for the trees' and bushes' leaves (stencil 136 and 137), which the game lights with its leaf ramp, and the diffuse
// irradiance there; zero on everything else. By the bloom step the stencil and the G-buffer hold other things.
#include "gbuffer.hlsli"

Texture2D<float2> shadow_mask : register(t0);   // the game's shadow mask: .x sun visibility
Texture2D<float4> albedo : register(t5);        // G-buffer rt1, sRGB-encoded as stored
Texture2D<float3> irradiance : register(t6);    // the diffuse irradiance (ambient pair rt1), ours when Ambient light is on

struct Leaf
{
    float4 color : SV_Target0;      // rgb the albedo as stored, a the sun's visibility
    float4 normal : SV_Target1;     // rgb the world normal * 0.5 + 0.5, a 1 for a tree's or bush's leaf, 0 for grass
    float4 light : SV_Target2;      // rgb the diffuse irradiance, which the game's leaf shader does not read
};

Leaf main(float4 position : SV_Position)
{
    int2 pixel = int2(position.xy);
    uint s = stencil.Load(int3(pixel, 0)).g;
    Leaf o = (Leaf)0;
    if (!is_foliage(s))
        return o;
    o.color = float4(albedo.Load(int3(pixel, 0)).rgb, shadow_mask.Load(int3(pixel, 0)).x);
    o.normal = float4(normals.Load(int3(pixel, 0)).rgb, s == 136 || s == 137 ? 1 : 0);
    o.light = float4(irradiance.Load(int3(pixel, 0)), 1);
    return o;
}
