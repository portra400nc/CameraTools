// The present pass: the sum's weighted average times accum_gain over the whole HDR scene, compressed by the highlight
// lift's inverse when the samples were expanded (lift.hlsli). A pixel no sample reached (the cat's eye can cut every
// sample at a corner) keeps the scene.
#include "common.hlsli"
#include "lift.hlsli"

Texture2D<float4> sum : register(t0);       // rgb: weighted HDR sum, a: weight sum
Texture2D<float3> scene : register(t1);     // mirror of the game's HDR scene
Texture2D<float> key : register(t2);        // the highlight lift's, lift_key_ps.hlsl's; unbound without the lift

float3 main(float4 position : SV_Position) : SV_Target0
{
    int3 pixel = int3(position.xy, 0);
    float4 s = sum.Load(pixel);
    return s.a > 0 ? lift_compress(s.rgb / s.a, lift_threshold(key.Load(int3(0, 0, 0))), accum_lift) * accum_gain : scene.Load(pixel);
}
