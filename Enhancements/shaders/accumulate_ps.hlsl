// The accumulation pass: this frame's HDR scene times the cat's-eye and swirl weights of its lens sample, and the weight
// in alpha, drawn with additive blending into our RGBA32F sum. Under chromatic aberration, red and blue are read along
// the lens point from the pixel, bilinear.
#include "common.hlsli"
#include "cat_eye.hlsli"

Texture2D<float3> scene : register(t0);     // mirror of the game's HDR scene

float4 main(float4 position : SV_Position) : SV_Target0
{
    float2 screen = stored_uv(position.xy * render_size.zw);
    float2 ndc = float2(screen.x * 2 - 1, 1 - screen.y * 2);
    float aspect = render_size.x * render_size.w;
    float weight = cat_eye_weight(lens_sample, ndc, aspect, cat_eye, cat_eye_falloff) * swirl_weight(lens_sample, ndc, aspect, accum_swirl, cat_eye_falloff);
    float3 c = scene.Load(int3(position.xy, 0));
    if (accum_chromatic.x != 0 || accum_chromatic.y != 0)
    {
        c.x = scene.SampleLevel(linear_clamp, stored_uv(chromatic_uv(screen, lens_sample, accum_chromatic.x, render_size.zw)), 0).x;
        c.z = scene.SampleLevel(linear_clamp, stored_uv(chromatic_uv(screen, lens_sample, accum_chromatic.y, render_size.zw)), 0).z;
    }
    return float4(c * weight, weight);
}
