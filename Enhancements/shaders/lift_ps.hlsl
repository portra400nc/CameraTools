// The highlight lift's draws over a lens sample's HDR scene (lift.hlsli), by accum_lift_pass: 0 the block means of its
// luminance, accum_lift_block render pixels a side, into a texture that size; 1 each block's gain against the brightest of
// 16 blocks on a ring four blocks out; 2 the largest gain of the 3 x 3 blocks around each; 3 the scene times the gain.
#include "common.hlsli"
#include "lift.hlsli"

Texture2D<float3> scene : register(t0);     // mirror of the game's HDR scene
Texture2D<float> small : register(t1);      // pass 0's, and pass 2's in pass 3
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
        float brightest = 0;
        [unroll] for (int k = 0; k < 16; k++)
        {
            float a = 6.2831853 * k / 16;
            int2 at = pixel + int2(round(4 * cos(a)), round(4 * sin(a)));
            brightest = max(brightest, small.Load(int3(clamp(at, 0, size - 1), 0)));
        }
        return lift_block_gain(small.Load(int3(pixel, 0)), brightest);
    }
    if (accum_lift_pass == 2)
    {
        int2 size;
        around.GetDimensions(size.x, size.y);
        float most = 1;
        [unroll] for (int y = -1; y <= 1; y++)
            [unroll] for (int x = -1; x <= 1; x++)
                most = max(most, around.Load(int3(clamp(pixel + int2(x, y), 0, size - 1), 0)));
        return most;
    }
    float2 uv = (position.xy) * render_size.zw;
    float reversed = depth.Load(int3(pixel, 0));
    float inverse = max(-(accum_lift_depth.x * reversed + accum_lift_depth.y) / (accum_lift_depth.z * reversed + accum_lift_depth.w), 0);
    float coc = accum_lift_coc.x * abs(accum_lift_coc.y - inverse);
    float gain = lift_faded(small.SampleLevel(linear_clamp, uv, 0), coc, render_size.y);
    return float4(scene.Load(int3(pixel, 0)) * gain, 1);
}
