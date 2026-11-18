// The character light pass's balance: a 1x1 target of the stops the world's light on characters is scaled by, and the
// stops of red and of blue over green their light's color moves by, eased each frame. The stops aim to bring the
// characters' light, as character_balance_ps.hlsl measured it, to traced.z times the world's around them, and only
// darken: where the world is lit brighter, the characters keep the game's light. The color aims kTintMatch of the way
// from the game's color of light on characters to the world's around them: the game lights characters in colors of its
// own, bluer at night than its world, so the world's light over the game's turned them orange under the physical moon.
// A frame with no character beside the world keeps the last.
#include "character_light.hlsli"

Texture2D<float4> meter : register(t0);     // character_balance_ps.hlsl's target with its whole mip chain
Texture2D<float4> previous : register(t1);  // the last frame's stops
Texture2D<float4> colors : register(t2);    // character_balance_ps.hlsl's color target with its whole mip chain

static const float kDarkest = -4;           // stops
static const float kTintMatch = 0.75;
static const float kTintLimit = 1.5;        // stops either way

float4 main(float4 position : SV_Position) : SV_Target0
{
    uint width, height, levels;
    meter.GetDimensions(0, width, height, levels);
    float4 mean = meter.Load(int3(0, 0, levels - 1));
    float4 tint = colors.Load(int3(0, 0, levels - 1));
    float4 last = previous.Load(int3(0, 0, 0));
    if (!(mean.y > 0 && mean.w > 0))
        return last;
    float stops = clamp(last.x + mean.x / mean.y - mean.z / mean.w + log2(traced.z), kDarkest, 0);
    // The characters' color as the game lit them is what was measured less the last frame's move.
    float2 game = tint.zw / mean.w - last.yz;
    float2 shift = clamp(kTintMatch * (tint.xy / mean.y - game), -kTintLimit, kTintLimit);
    return last + (float4(stops, shift, 0) - last) * traced.w;
}
