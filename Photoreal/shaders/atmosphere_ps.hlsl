// The atmosphere pass: aerial perspective over the HDR scene, before the game's bloom and tone map read it. Writes the
// whole target, because the result is copied over the game's HDR scene whole; the sky (depth 0) keeps the game's color.
#include "gbuffer.hlsli"

Texture2D<float3> scene : register(t0);     // mirror of the game's HDR scene

static const float kPi = 3.14159265;

// Henyey-Greenstein, normalized over the sphere.
float phase(float cosine, float g)
{
    float denominator = 1 + g * g - 2 * g * cosine;
    return (1 - g * g) / (4 * kPi * denominator * sqrt(denominator));
}

// The integral of exp(-height_falloff * rise * t) dt for t from 0 to distance: the ray's length weighted by the haze's
// density relative to the camera's height. rise is the ray's upward component per meter.
float haze_length(float distance, float rise)
{
    float x = max(height_falloff * rise * distance, -80);   // far below the camera the haze is opaque anyway
    if (abs(x) < 1e-3)
        return distance * (1 - 0.5 * x);
    return distance * (1 - exp(-x)) / x;
}

float3 main(float4 position : SV_Position) : SV_Target0
{
    int2 pixel = int2(position.xy);
    float3 color = scene.Load(int3(pixel, 0));
    if (depth.Load(int3(pixel, 0)) <= 0)
        return color;
    float3 p = view_position_at(pixel);
    float distance = length(p);
    float3 ray = p / distance;
    float rise = mul(ray, (float3x3)world_to_view).y;       // world_to_view is orthogonal, so its transpose takes view to world
    float transmittance = exp(-atmosphere_density * haze_length(distance, rise));
    float3 l = mul((float3x3)world_to_view, toward_sun);
    float3 sun = dot(l, l) > 0 ? sun_color * sun_scatter * phase(dot(ray, l), anisotropy) : 0;
    return color * transmittance + (sky_color + sun) * (1 - transmittance);
}
