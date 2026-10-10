// The leaves pass: the sun light leaves and grass let through toward the camera, drawn additively into a mirror of the HDR
// scene after the game's deferred lighting. Every other pixel adds nothing.
#include "gbuffer.hlsli"

Texture2D<float2> shadow_mask : register(t0);   // mirror of the game's shadow mask; the sun's visibility in .x
Texture2D<float4> albedo : register(t5);        // mirror of G-buffer rt1, sRGB-encoded as stored
Texture2D<float> material : register(t6);       // mirror of G-buffer rt3: the material id / 255, with flags in bits 6 and 7

float3 srgb_to_linear(float3 c) { return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4); }

float3 main(float4 position : SV_Position) : SV_Target0
{
    int2 pixel = int2(position.xy);
    uint id = uint(material.Load(int3(pixel, 0)) * 255 + 0.5) & ~0xC0u;
    // Leaves are ids 2 and 15, grass 3, as the game's vegetation lighting draw tests them.
    if (!is_foliage(stencil.Load(int3(pixel, 0)).g) || (id != 2 && id != 15 && id != 3))
        return 0;
    float3 l = mul((float3x3)world_to_view, toward_sun);
    float3 toward_pixel = normalize(view_position_at(pixel));
    float3 n = view_normal_at(pixel);
    // Bright where the camera looks toward the sun, and only on the side of the leaf away from it: the game's wrapped
    // diffuse already lights back faces dimly and uncolored, so front-lit leaves get nothing more.
    float forward_scatter = pow(saturate(dot(toward_pixel, l)), leaves_scatter_sharpness) * saturate(-dot(n, l) * 0.5 + 0.5);
    return srgb_to_linear(albedo.Load(int3(pixel, 0)).rgb) * sun_color * shadow_mask.Load(int3(pixel, 0)).x * leaves_strength * forward_scatter;
}
