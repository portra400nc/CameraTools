// The trace pass's draw at the combine step, after its dispatch: from the history's averages, the sun visibility into the
// shadow mask's .x and the occlusion into the diffuse irradiance, on world pixels with a sample, and the game's values
// everywhere else, because each result is copied over the game's buffer whole. Below 16 samples a pixel takes the
// edge-aware 4x4 blur of its neighbors' averages, so the first passes' noise does not show; from 16 on its own. The third
// target, bound only for the trace view, gets the raw averages: occlusion red, sun green, blue where there is no sample.
#include "gbuffer.hlsli"

cbuffer Trace : register(b1)
{
    float4 clip_rows[4];
    float4 eye_position;
    float4 sun_cone;
    float4 trace_params;    // ao_distance, ao_strength, sun_strength, ambient level
    uint4 trace_counts;
    uint4 trace_tiles;
    uint4 trace_render;     // width, height, flip, grid stride
};

Texture2D<float2> shadow_mask : register(t0);       // mirror of the game's shadow mask: .x sun visibility, .y untouched
StructuredBuffer<float4> history : register(t5);    // the trace's: occlusion sum, sun sum, their counts, row by row
Texture2D<float3> irradiance : register(t6);        // mirror of the game's diffuse irradiance

static const float kBlurBelow = 16;

struct Targets
{
    float2 mask : SV_Target0;
    float3 irradiance : SV_Target1;
    float4 view : SV_Target2;
};

float4 history_at(int2 q) { return history[uint(q.y) * trace_render.x + uint(q.x)]; }

// The 4x4 blur of the neighbors' averages, occlusion in .x and sun in .y, weighted as blurred() in gbuffer.hlsli, each
// over the taps with a sample of it.
float2 blurred_averages(int2 pixel, float z, float3 n)
{
    float2 sum = 0, total = 0;
    [unroll] for (int y = -2; y < 2; y++)
    {
        [unroll] for (int x = -2; x < 2; x++)
        {
            int2 q = pixel + int2(x, y);
            float weight = blur_weight(q, z, n);
            if (weight <= 0)
                continue;
            float4 h = history_at(q);
            if (h.z > 0)
            {
                sum.x += h.x / h.z * weight;
                total.x += weight;
            }
            if (h.w > 0)
            {
                sum.y += h.y / h.w * weight;
                total.y += weight;
            }
        }
    }
    return sum / max(total, 1e-6);  // the center has its samples and counts with weight 1
}

Targets main(float4 position : SV_Position)
{
    int2 pixel = int2(position.xy);
    Targets o;
    float2 game_mask = shadow_mask.Load(int3(pixel, 0));
    o.mask = game_mask;
    o.irradiance = irradiance.Load(int3(pixel, 0));
    float4 h = history_at(pixel);
    o.view = float4(h.z > 0 ? h.x / h.z : 0, h.w > 0 ? h.y / h.w : 0, h.z > 0 || h.w > 0 ? 0 : 1, 1);
    if (!is_world(stencil.Load(int3(pixel, 0)).g))
        return o;
    float2 averages = float2(h.z > 0 ? h.x / h.z : 1, h.w > 0 ? h.y / h.w : 1);
    if ((h.z > 0 && h.z < kBlurBelow) || (h.w > 0 && h.w < kBlurBelow))
    {
        float2 blurred = blurred_averages(pixel, view_position_at(pixel).z, view_normal_at(pixel));
        averages = float2(h.z < kBlurBelow ? blurred.x : averages.x, h.w < kBlurBelow ? blurred.y : averages.y);
    }
    if (h.w > 0)
        o.mask = float2(lerp(1, averages.y, trace_params.z), game_mask.y);
    if (h.z > 0)
        o.irradiance *= trace_params.w * lerp(1, averages.x, trace_params.y);
    return o;
}
