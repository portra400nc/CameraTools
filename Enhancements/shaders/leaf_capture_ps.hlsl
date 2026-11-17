// Light through leaves' copy at the combine step, after our passes there: each grass and leaf pixel's albedo as stored and
// the sun's visibility in the shadow mask, which the game's shadow map and our shadow passes lowered; zero on everything
// else. By the bloom step the stencil and the G-buffer hold other things.
#include "gbuffer.hlsli"

Texture2D<float2> shadow_mask : register(t0);   // the game's shadow mask: .x sun visibility
Texture2D<float4> albedo : register(t5);        // G-buffer rt1, sRGB-encoded as stored

float4 main(float4 position : SV_Position) : SV_Target0
{
    int2 pixel = int2(position.xy);
    if (!is_foliage(stencil.Load(int3(pixel, 0)).g))
        return 0;
    return float4(albedo.Load(int3(pixel, 0)).rgb, shadow_mask.Load(int3(pixel, 0)).x);
}
