// The reflections' draw at the combine step: the traced mean over the probes the game's combine pass reads, and the smoothness
// the rays used, so that pass's reflection weight and its highlights agree with them. A pixel far brighter than its neighbors
// comes back to them: a lone ray that met a light source. Other pixels keep the game's.
#include "common.hlsli"
#include "reflect_common.hlsli"

cbuffer Reflect : register(b4)
{
    float4 reflect_params;
};

StructuredBuffer<float4> reflect_sum : register(t0);
Texture2D<float3> probes : register(t1);        // mirror of the game's reflections
Texture2D<float> smoothness : register(t2);     // mirror of G-buffer rt4
Texture2D<uint2> stencil : register(t3);

struct Targets
{
    float3 reflection : SV_Target0;
    float smoothness : SV_Target1;
};

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

Targets main(float4 position : SV_Position)
{
    int2 pixel = int2(position.xy);
    Targets o;
    o.reflection = probes.Load(int3(pixel, 0));
    o.smoothness = smoothness.Load(int3(pixel, 0));
    bool has;
    float3 mean = mean_at(pixel, has);
    if (!has)
        return o;
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
                around += dot(m, float3(0.2126, 0.7152, 0.0722));
                count += 1;
            }
        }
    float seen = dot(mean, float3(0.2126, 0.7152, 0.0722));
    float limit = count > 0 ? 3 * around / count + 0.02 : seen;
    o.reflection = seen > limit ? mean * (limit / seen) : mean;
    // Never rougher than the game stored it: its own highlights keep their sharpness.
    o.smoothness = max(o.smoothness, 1 - reflect_roughness_of(o.smoothness, reflect_params.x));
    return o;
}
