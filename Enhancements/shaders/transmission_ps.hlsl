// Leaves at the bloom step, from leaf_capture_ps.hlsl's copy at the combine step: Leaf shading relights trees' and bushes'
// leaves, and Light through leaves adds the sun's light that grass and leaves let through.
//
// Leaf shading. The game lights a leaf with a ramp of the sun, clamp(0.36 N.L + 0.54, 0.25, 0.75), barely shadowed, and
// light of its own, ignoring the diffuse irradiance and the shadow mask that light every other surface: flat, banded and
// bright inside the canopy. The pass lights trees' and bushes' leaves as the game lights the rest of the world, from the
// irradiance (ours with Ambient light, occluded deeper in a canopy) and the sun through the shadow mask, which the
// canopy's own leaves darken inside, by a wrapped Lambert, as a canopy's spread leaf angles soften it. In traced shots
// both are the trace's: its occlusion and bounce in the irradiance, its sun visibility in the mask.
//
// Light through leaves. A leaf between the camera and the sun glows most, in a narrow forward lobe, and a little at any
// angle the sun reaches its far side; the light comes out tinted twice by the leaf, as it passes through its color.
#include "common.hlsli"

Texture2D<float3> scene : register(t0);     // mirror of the HDR scene
Texture2D<float> depth : register(t2);      // mirror of the main depth, reversed Z
Texture2D<float4> leaf : register(t4);      // rgb the leaf's albedo as stored, a its sunlight; zero where no leaf
Texture2D<float4> leaf_normal : register(t5);   // rgb the world normal * 0.5 + 0.5, a 1 for a tree's or bush's leaf
Texture2D<float3> leaf_light_in : register(t6); // the diffuse irradiance on the leaf

static const float kThrough = 1.5;          // the sun a leaf passes on, at strength 1, against its lit side's
static const float kWrap = 0.5;             // the wrapped Lambert's: (N.L + w) / (1 + w)
// The game paints leaves brighter and more saturated than real ones, which its leaf ramp hid and physical light shows:
// measured on a pair of traced shots, the canopy came out 1.5 times as bright and saturation 0.66 against 0.55. A
// leaf's color loses up to this much of its brightness and of its saturation, in proportion to how saturated it is, so
// green leaves take it all and white or pale flowers, drawn with the leaves, keep their color.
static const float kLeafDarken = 0.35, kLeafDesaturate = 0.3;

// The leaf's color as a real leaf's would be: darker and less saturated by its own saturation.
float3 leaf_albedo(float3 albedo)
{
    float high = max(albedo.r, max(albedo.g, albedo.b)), low = min(albedo.r, min(albedo.g, albedo.b));
    float saturation = high > 1e-4 ? (high - low) / high : 0;
    float lum = dot(albedo, float3(0.2126, 0.7152, 0.0722));
    return lerp(albedo, lum, kLeafDesaturate * saturation) * (1 - kLeafDarken * saturation);
}

float3 srgb_to_linear(float3 c) { return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4); }

float4 main(float4 position : SV_Position) : SV_Target0
{
    int2 pixel = int2(position.xy);
    float3 color = scene.Load(int3(pixel, 0));
    float4 l = leaf.Load(int3(pixel, 0));
    float d = depth.Load(int3(pixel, 0));
    if (all(l.rgb <= 0) || d <= 0 || dot(toward_sun, toward_sun) < 0.5)
        return float4(color, 1);
    float3 albedo = srgb_to_linear(l.rgb);
    float4 nl = leaf_normal.Load(int3(pixel, 0));
    if (leaf_light > 0 && nl.a > 0.5)
    {
        float3 n = normalize(nl.rgb * 2 - 1);
        float sun = saturate((dot(n, toward_sun) + kWrap) / (1 + kWrap)) * l.a;
        float3 lit = leaf_albedo(albedo) * (leaf_light_in.Load(int3(pixel, 0)) + sun_color * sun);
        color = lerp(color, lit, saturate(leaf_light));
    }
    if (transmission_strength > 0 && l.a > 0)
    {
        float2 uv = position.xy * render_size.zw;
        float3 away = normalize(mul(transpose((float3x3)world_to_view), view_position(uv, d)));
        float back = saturate(dot(away, toward_sun));
        float glow = 0.6 * pow(back, 4) + 0.4 * back;
        float3 tint = albedo * albedo / max(dot(albedo, 1.0 / 3), 0.02);
        color += tint * sun_color * l.a * glow * kThrough * transmission_strength;
    }
    return float4(color, 1);
}
