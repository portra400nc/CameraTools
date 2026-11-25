// The highlight lift's draws over a lens sample's HDR scene (lift.hlsli), by accum_lift_pass: 0 the block means of its
// luminance, accum_lift_block render pixels a side, into a texture that size; 1 the 7 x 7 mean of those around each
// block; 2 the scene times the gain.
#include "common.hlsli"
#include "lift.hlsli"

Texture2D<float3> scene : register(t0);     // mirror of the game's HDR scene
Texture2D<float> small : register(t1);      // pass 0's
Texture2D<float> around : register(t2);     // pass 1's
Texture2D<float> depth : register(t3);      // mirror of the main depth, reversed

float4 main(float4 position : SV_Position) : SV_Target0
{
    int2 pixel = int2(position.xy);
    if (accum_lift_pass == 0)
    {
        int block = int(accum_lift_block);
        int2 corner = pixel * block, size = int2(render_size.xy);
        float sum = 0, count = 0;
        [loop] for (int y = 0; y < block; y++)
            [loop] for (int x = 0; x < block; x++)
            {
                int2 at = corner + int2(x, y);
                if (all(at < size))
                {
                    sum += dot(scene.Load(int3(at, 0)), kLiftLuma);
                    count += 1;
                }
            }
        return sum / max(count, 1);
    }
    if (accum_lift_pass == 1)
    {
        int2 size;
        small.GetDimensions(size.x, size.y);
        float sum = 0, count = 0;
        [unroll] for (int y = -3; y <= 3; y++)
            [unroll] for (int x = -3; x <= 3; x++)
            {
                sum += small.Load(int3(clamp(pixel + int2(x, y), 0, size - 1), 0));
                count += 1;
            }
        return sum / count;
    }
    float2 uv = (position.xy) * render_size.zw;
    float reversed = depth.Load(int3(pixel, 0));
    float inverse = max(-(accum_lift_depth.x * reversed + accum_lift_depth.y) / (accum_lift_depth.z * reversed + accum_lift_depth.w), 0);
    float coc = accum_lift_coc.x * abs(accum_lift_coc.y - inverse);
    float gain = lift_gain(small.SampleLevel(linear_clamp, uv, 0), around.SampleLevel(linear_clamp, uv, 0), coc, render_size.y);
    return float4(scene.Load(int3(pixel, 0)) * gain, 1);
}
