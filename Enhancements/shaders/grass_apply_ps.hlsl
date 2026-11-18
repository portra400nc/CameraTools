// The grass pass's second draw: the HDR scene times grass_ps.hlsl's factor averaged over each grass or ground pixel's
// 3x3 neighbors that have one. Far grass is dithered blade and ground pixel by pixel, and a factor that darkened only the
// ground showed the dither as a checker. Writes the whole target, because the result is copied over the HDR scene whole.
#include "common.hlsli"

Texture2D<float3> scene : register(t0);     // mirror of the HDR scene
Texture2D<float4> factor : register(t4);    // grass_ps.hlsl's: rgb the factor, a 1 on grass and ground

float4 main(float4 position : SV_Position) : SV_Target0
{
    int2 pixel = int2(position.xy);
    float3 color = scene.Load(int3(pixel, 0));
    if (factor.Load(int3(pixel, 0)).a <= 0)
        return float4(color, 1);
    float4 sum = 0;
    [unroll] for (int y = -1; y <= 1; y++)
        [unroll] for (int x = -1; x <= 1; x++)
        {
            float4 f = factor.Load(int3(clamp(pixel + int2(x, y), 0, int2(render_size.xy) - 1), 0));
            sum += float4(f.rgb * f.a, f.a);
        }
    return float4(color * sum.rgb / sum.a, 1);
}
