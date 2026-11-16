// The scene's light: the tone map's output lifted, each channel x to x g / (1 + (g - 1) x), so black and white stay and
// the tones between brighten, most in the shadows and mid tones.
#include "common.hlsli"

Texture2D<float4> picture : register(t0);   // mirror of the tone map's output

float4 main(float4 position : SV_Position) : SV_Target0
{
    float4 c = picture.Load(int3(position.xy, 0));
    float g = scene_gain;
    c.rgb = c.rgb * g / (1 + (g - 1) * saturate(c.rgb));
    return c;
}
