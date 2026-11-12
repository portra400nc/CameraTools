// A DNG's light, drawn at the game's tone map draw beside it into a target of ours: the image the DNG develops plus the game's
// final bloom through the game's color matrix and exposure, as tonemap_ps.hlsl's steps 1 and 2 compute them, and no curve.
// bloom_strength and exposure_scale are 1, and the auto exposure unbound, when our tonemap pass does not draw the frame
// (core/dng.h's dng_grade).
#include "common.hlsli"

Texture2D<float4> image : register(t0);     // the HDR scene or the accumulator's average, with the depth of field when it runs
Texture2D<float3> bloom : register(t1);     // mirror of the game's final bloom; unbound when the tone map draw adds none
Texture2D<float> auto_ev : register(t2);    // exposure_adapt_ps.hlsl's stops; unbound, 0, while the auto exposure is off

#include "grade.hlsli"

float4 main(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target0
{
    Grade g = game_grade();
    float3 hdr = image.Load(int3(position.xy, 0)).rgb + bloom.SampleLevel(linear_clamp, uv, 0) * (g.bloom * bloom_strength);
    return float4(mul(hdr, g.color) * (g.exposure * exposure_scale * exp2(auto_ev.Load(int3(0, 0, 0)))), 1);
}
