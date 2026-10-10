// The accumulation pass: this frame's HDR scene times the cat's-eye weight of its lens sample, and the weight in alpha,
// drawn with additive blending into our RGBA32F sum.
#include "common.hlsli"
#include "cat_eye.hlsli"

Texture2D<float3> scene : register(t0);     // mirror of the game's HDR scene

float4 main(float4 position : SV_Position) : SV_Target0
{
    float2 screen = stored_uv(position.xy * render_size.zw);
    float2 ndc = float2(screen.x * 2 - 1, 1 - screen.y * 2);
    float weight = cat_eye_weight(lens_sample, ndc, render_size.x * render_size.w, cat_eye, cat_eye_falloff);
    return float4(scene.Load(int3(position.xy, 0)) * weight, weight);
}
