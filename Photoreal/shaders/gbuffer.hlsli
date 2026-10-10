// What every pass reads besides its own target: the G-buffer at t1 to t3, the raw result of its first draw at t4, the
// per-pixel pattern its first draw rotates by, and the 4x4 blur its second draw averages that pattern away with.
#include "common.hlsli"

Texture2D<float4> normals : register(t1);       // mirror of G-buffer rt0, world normal * 0.5 + 0.5
Texture2D<float> depth : register(t2);          // mirror of the main depth, R32_FLOAT_X8X24_TYPELESS, reversed Z
Texture2D<uint2> stencil : register(t3);        // same mirror, X32_TYPELESS_G8X24_UINT; stencil in .g
Texture2D<float> raw : register(t4);            // the first draw's output, bound for the second draw only

float3 view_position_at(int2 pixel) { return view_position((pixel + 0.5) * render_size.zw, depth.Load(int3(pixel, 0))); }

float3 view_normal_at(int2 pixel) { return normalize(mul((float3x3)world_to_view, normals.Load(int3(pixel, 0)).xyz * 2 - 1)); }

// The 4x4 Bayer order, (0 8 2 10 / 12 4 14 6 / 3 11 1 9 / 15 7 13 5) / 16. Any 4x4 window of pixels holds each of the
// 16 values once, so one blur window sees 16 offsets, evenly spread.
float pattern(int2 pixel)
{
    uint2 p = uint2(pixel) & 3u;
    uint z = p.x ^ p.y;
    return ((z & 1u) * 8 + (p.y & 1u) * 4 + (z >> 1) * 2 + (p.y >> 1) + 0.5) / 16;
}

// A tap's weight falls to 0 at this view-depth difference relative to the center's depth. Ground seen at 5 degrees
// differs about 4% over two pixels at 720p and keeps most of its weight; a branch in front of a wall drops out.
static const float kDepthTolerance = 0.1;

// A 4x4 window, one whole period of pattern(), so a flat surface averages its 16 offsets exactly. Taps that are not
// world (sky, characters) are skipped, and taps across a depth jump or a sharp normal change fade out.
float blurred(int2 pixel, float z, float3 n)
{
    float sum = 0, total = 0;
    [unroll] for (int y = -2; y < 2; y++)
    {
        [unroll] for (int x = -2; x < 2; x++)
        {
            int2 q = pixel + int2(x, y);
            if (any(q < 0) || any(q >= int2(render_size.xy)) || !is_world(stencil.Load(int3(q, 0)).g))
                continue;
            float depth_weight = saturate(1 - abs(view_position_at(q).z - z) / (max(-z, 0.05) * kDepthTolerance));
            float facing = dot(n, view_normal_at(q)) * 0.5 + 0.5;
            float weight = depth_weight * facing * facing;
            sum += raw.Load(int3(q, 0)) * weight;
            total += weight;
        }
    }
    return sum / total;     // the center is world and counts with weight 1
}
