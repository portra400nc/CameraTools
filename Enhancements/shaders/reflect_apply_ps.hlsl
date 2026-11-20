// The reflections' draw at the combine step: the traced mean over the probes the game's combine pass reads, and the smoothness
// the rays used, so that pass's reflection weight and its highlights agree with them. Each pixel holds the mean of a few
// dozen rays, too few for a rough lobe, so the mean is filtered over its neighbors on the same surface (blur_weight) with a
// gaussian whose width grows with the roughness: a mirror stays sharp, rough wood averages its window. A tap far brighter than
// the pixel's 3x3 comes back to it first: a lone ray that met a light source. Other pixels keep the game's.
#include "gbuffer.hlsli"
#include "reflect_common.hlsli"

cbuffer Reflect : register(b4)
{
    float4 reflect_params;
};

StructuredBuffer<float4> reflect_sum : register(t0);
Texture2D<float3> probes : register(t5);        // mirror of the game's reflections
Texture2D<float> smoothness : register(t6);     // mirror of G-buffer rt4

struct Targets
{
    float3 reflection : SV_Target0;
    float smoothness : SV_Target1;
};

static const int kRadius = 4;
// The gaussian's width in pixels per unit of roughness, and its cap. Even a glossy floor's lobe (0.3) spans degrees, wider
// than this window at 4K, so the window blurs no reflected detail the lobe keeps.
static const float kSigmaAtRough = 12.0;
static const float kSigmaMax = 4.0;

float luminance_of(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }

float3 mean_at(int2 pixel, out bool has)
{
    has = false;
    int2 size = int2(render_size.xy);
    if (pixel.x < 0 || pixel.y < 0 || pixel.x >= size.x || pixel.y >= size.y)
        return float3(0, 0, 0);
    float4 s = reflect_sum[uint(pixel.y) * uint(size.x) + uint(pixel.x)];
    has = s.w > 0;
    return has ? s.rgb / s.w : float3(0, 0, 0);
}

float rough_at(int2 pixel) { return reflect_roughness_of(smoothness.Load(int3(pixel, 0)), reflect_params.x); }

// A tap's weight: on the same surface, near in roughness, and within the gaussian. 0 for none.
float tap_weight(int2 q, int2 offset, float z, float3 n, float rough, float sigma, out float3 mean)
{
    mean = 0;
    bool has;
    float3 m = mean_at(q, has);
    if (!has)
        return 0;
    float w = blur_weight(q, z, n);
    if (w <= 0)
        return 0;
    float apart = abs(rough_at(q) - rough);
    mean = m;
    return w * exp(-dot(float2(offset), float2(offset)) / (2 * sigma * sigma)) * saturate(1 - apart / 0.25);
}

Targets main(float4 position : SV_Position)
{
    int2 pixel = int2(position.xy);
    Targets o;
    o.reflection = probes.Load(int3(pixel, 0));
    o.smoothness = smoothness.Load(int3(pixel, 0));
    bool has;
    float3 own = mean_at(pixel, has);
    if (!has)
        return o;
    float rough = rough_at(pixel);
    float sigma = min(kSigmaAtRough * rough, kSigmaMax);
    float3 reflection = own;
    if (sigma > 0.75 && is_world(stencil.Load(int3(pixel, 0)).g))
    {
        float z = view_position_at(pixel).z;
        float3 n = view_normal_at(pixel);
        // 9x9 taps, spread over twice the gaussian's width.
        int spacing = sigma > 2 ? 2 : 1;
        int radius = min(kRadius, int(ceil(2 * sigma / spacing)));
        // The limit from the 3x3 around: a tap brighter than 3 times their mean comes down to it.
        float seen = 0, count = 0;
        [unroll] for (int j = -1; j <= 1; j++)
            [unroll] for (int i = -1; i <= 1; i++)
            {
                bool other;
                float3 m = mean_at(pixel + int2(i, j), other);
                if (other)
                {
                    seen += luminance_of(m);
                    count += 1;
                }
            }
        float limit = 3 * seen / count + 0.02;
        float3 sum = 0;
        float total = 0;
        [loop] for (int y = -radius; y <= radius; y++)
            [loop] for (int x = -radius; x <= radius; x++)
            {
                int2 offset = int2(x, y) * spacing;
                float3 m;
                float w = tap_weight(pixel + offset, offset, z, n, rough, sigma, m);
                float l = luminance_of(m);
                sum += (l > limit ? m * (limit / l) : m) * w;
                total += w;
            }
        if (total > 0)
            reflection = sum / total;
    }
    else
    {
        // A mirror: only the lone bright pixel comes back to its neighbors.
        float around = 0, count = 0;
        for (int y = -1; y <= 1; y++)
            for (int x = -1; x <= 1; x++)
            {
                if (x == 0 && y == 0)
                    continue;
                bool other;
                float3 m = mean_at(pixel + int2(x, y), other);
                if (other)
                {
                    around += luminance_of(m);
                    count += 1;
                }
            }
        float l = luminance_of(own);
        float limit = count > 0 ? 3 * around / count + 0.02 : l;
        reflection = l > limit ? own * (limit / l) : own;
    }
    o.reflection = reflection;
    // Never rougher than the game stored it: its own highlights keep their sharpness.
    o.smoothness = max(o.smoothness, 1 - rough);
    return o;
}
