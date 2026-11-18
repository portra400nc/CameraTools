// The character light pass's balance: a 1x1 target of the stops the world's light on characters is scaled by, eased each
// frame toward what brings the characters' light, as character_balance_ps.hlsl measured it, to traced.z times the
// world's around them. It only darkens: where the world is lit brighter, the characters keep the game's light. A frame
// with no character beside the world keeps the last.
#include "character_light.hlsli"

Texture2D<float4> meter : register(t0);     // character_balance_ps.hlsl's target with its whole mip chain
Texture2D<float> previous : register(t1);   // the last frame's stops

static const float kDarkest = -4;           // stops

float main(float4 position : SV_Position) : SV_Target0
{
    uint width, height, levels;
    meter.GetDimensions(0, width, height, levels);
    float4 mean = meter.Load(int3(0, 0, levels - 1));
    float last = previous.Load(int3(0, 0, 0));
    if (!(mean.y > 0 && mean.w > 0))
        return last;
    float target = clamp(last + mean.x / mean.y - mean.z / mean.w + log2(traced.z), kDarkest, 0);
    return last + (target - last) * traced.w;
}
