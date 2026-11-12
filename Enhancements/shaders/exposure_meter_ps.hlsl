// Auto exposure's meter: each pixel of a 64x32 target takes 4x4 points of its share of the HDR scene and writes the sum of
// their log2 luminance and their count, the sky's points (reversed-Z depth 0) left out. The target's mips then average it.
#include "common.hlsli"
#include "exposure.hlsli"

Texture2D<float3> scene : register(t0);     // mirror of the game's HDR scene
Texture2D<float> depth : register(t1);      // mirror of the main depth, reversed Z; unbound, every point counts

float2 main(float4 position : SV_Position) : SV_Target0
{
    float2 block = render_size.xy / float2(64, 32);
    float2 corner = floor(position.xy) * block;
    uint width, height, levels;
    depth.GetDimensions(0, width, height, levels);
    float sum = 0, count = 0;
    [unroll] for (int y = 0; y < 4; y++)
        [unroll] for (int x = 0; x < 4; x++)
        {
            int3 p = int3(corner + (float2(x, y) + 0.5) * block / 4, 0);
            if (width > 0 && depth.Load(p) <= 0)
                continue;
            sum += exposure_log_luminance(scene.Load(p));
            count += 1;
        }
    return float2(sum, count) / 16;
}
