// Light through leaves at the bloom step: the sun's light that grass and leaves let through, added to them in the HDR
// scene. A leaf between the camera and the sun glows most, in a narrow forward lobe, and a little at any angle the sun
// reaches its far side; the light comes out tinted twice by the leaf, as it passes through its color. The leaves, their
// colors and their sunlight come from leaf_capture_ps.hlsl at the combine step.
#include "common.hlsli"

Texture2D<float3> scene : register(t0);     // mirror of the HDR scene
Texture2D<float> depth : register(t2);      // mirror of the main depth, reversed Z
Texture2D<float4> leaf : register(t4);      // rgb the leaf's albedo as stored, a its sunlight; zero where no leaf

static const float kThrough = 1.5;          // the sun a leaf passes on, at strength 1, against its lit side's

float3 srgb_to_linear(float3 c) { return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4); }

float4 main(float4 position : SV_Position) : SV_Target0
{
    int2 pixel = int2(position.xy);
    float3 color = scene.Load(int3(pixel, 0));
    float4 l = leaf.Load(int3(pixel, 0));
    float d = depth.Load(int3(pixel, 0));
    if (all(l.rgb <= 0) || l.a <= 0 || d <= 0 || dot(toward_sun, toward_sun) < 0.5)
        return float4(color, 1);
    float2 uv = position.xy * render_size.zw;
    float3 away = normalize(mul(transpose((float3x3)world_to_view), view_position(uv, d)));
    float back = saturate(dot(away, toward_sun));
    float glow = 0.6 * pow(back, 4) + 0.4 * back;
    float3 tint = srgb_to_linear(l.rgb);
    tint *= tint / max(dot(tint, 1.0 / 3), 0.02);
    return float4(color + tint * sun_color * l.a * glow * kThrough * transmission_strength, 1);
}
