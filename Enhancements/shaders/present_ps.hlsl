// The present pass: the sum's weighted average over the whole HDR scene. A pixel no sample reached (the cat's eye can cut
// every sample at a corner) keeps the scene.
#include "common.hlsli"

Texture2D<float4> sum : register(t0);       // rgb: weighted HDR sum, a: weight sum
Texture2D<float3> scene : register(t1);     // mirror of the game's HDR scene

float3 main(float4 position : SV_Position) : SV_Target0
{
    int3 pixel = int3(position.xy, 0);
    float4 s = sum.Load(pixel);
    return s.a > 0 ? s.rgb / s.a : scene.Load(pixel);
}
