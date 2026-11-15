// A star's share of light in one of its 3x3 pixels, added to the sky's pixels only (stars_vs.hlsl).
#include "common.hlsli"

Texture2D<float> depth : register(t2);  // mirror of the main depth, reversed Z

static const float kStarSpread = 0.6;

float3 main(float4 position : SV_Position, nointerpolation float2 center : TEXCOORD0, nointerpolation float3 light : TEXCOORD1) : SV_Target0
{
    if (depth.Load(int3(position.xy, 0)) > 0)
        discard;
    float2 d = position.xy - center;
    return light * exp(-dot(d, d) / (2 * kStarSpread * kStarSpread));
}
