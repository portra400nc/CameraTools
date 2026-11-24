// The trace pass's draw at the combine step, after its dispatch: from the history's averages, the sun visibility into the
// shadow mask's .x and the occlusion into the diffuse irradiance, on world pixels with a sample, and the game's values
// everywhere else, because each result is copied over the game's buffer whole. Below 16 samples a pixel takes the
// edge-aware 4x4 blur of its neighbors' averages, so the first passes' noise does not show; from 16 on its own. The third
// target, bound only for the trace view, gets the raw averages: occlusion red, sun green, blue where there is no sample.
// The fourth gets grass's and leaves' shade for the traced foliage pass at bloom, which the game's foliage lighting does
// not take from the irradiance and the mask: just under 1 at the most on foliage, and 1 elsewhere.
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
Texture2D<float> material : register(t7);           // mirror of rt3, the material id / 255; unbound, 0

static const uint kTallGrassMaterial = 3;           // tall grass, which the game draws under the leaves' stencil 137

static const float kBlurBelow = 16;

struct Targets
{
    float2 mask : SV_Target0;
    float3 irradiance : SV_Target1;
    float4 view : SV_Target2;
    float2 foliage : SV_Target3;    // the shade, and 1 where the traced foliage pass blurs it (grass)
};

// The share of the traced sun's shadow that the foliage shade takes. Grass takes a little, as blades shading blades drew
// dark lines; bushes and trees most, as the game lights their leaves its own way, past the shadow mask, so a bush in traced
// shade otherwise stayed as bright as one in the sun beside darkened grass.
static const float kGrassSunShare = 0.35, kLeafSunShare = 0.8;
// How deep the traced sun's shadow on grass and leaves goes in the mask, against 1 elsewhere.
static const float kFoliageSunShadow = 0.5;

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
    o.foliage = float2(1, 0);
    uint s = stencil.Load(int3(pixel, 0)).g;
    if (!is_world(s))
        return o;
    float2 averages = float2(h.z > 0 ? h.x / h.z : 1, h.w > 0 ? h.y / h.w : 1);
    // trace_tiles.z is 1 for an anchored shot's views and 2 for their moves, which show each pixel's own samples.
    if (trace_tiles.z == 0 && ((h.z > 0 && h.z < kBlurBelow) || (h.w > 0 && h.w < kBlurBelow)))
    {
        float2 blurred = blurred_averages(pixel, view_position_at(pixel).z, view_normal_at(pixel));
        averages = float2(h.z < kBlurBelow ? blurred.x : averages.x, h.w < kBlurBelow ? blurred.y : averages.y);
    }
    bool foliage = is_foliage(s);
    bool grass = foliage && (is_grass(s) || (uint(material.Load(int3(pixel, 0)) * 255 + 0.5) & 0x3Fu) == kTallGrassMaterial);
    if (h.w > 0 && grass)
    {
        // A blade lets sunlight through, and its shadows on the next are soft: the traced sun, blurred, at half its
        // strength. Shadows the game's own shadow map casts, as a tree's on the grass, stay whole.
        float sun = blurred_averages(pixel, view_position_at(pixel).z, view_normal_at(pixel)).y;
        o.mask = float2(min(game_mask.x, lerp(1, sun, trace_params.z * kFoliageSunShadow)), game_mask.y);
    }
    else if (h.w > 0 && foliage)
    {
        // A tree's or bush's leaf keeps its own traced sun, at the share the traced foliage shade gives leaves: a
        // canopy's leaves sit at about one depth, so a blur smeared the shade of inner leaves over the edges of the
        // ones in front, which Leaf shading, lighting leaves through the mask, showed.
        o.mask = float2(min(game_mask.x, lerp(1, averages.y, trace_params.z * kLeafSunShare)), game_mask.y);
    }
    else if (h.w > 0)
        o.mask = float2(lerp(1, averages.y, trace_params.z), game_mask.y);
    if (h.z > 0)
        o.irradiance *= trace_params.w * lerp(1, averages.x, trace_params.y);
    if (foliage)
    {
        o.foliage = float2(min(lerp(1, h.z > 0 ? averages.x : 1, trace_params.y) * lerp(1, h.w > 0 ? averages.y : 1, trace_params.z * (grass ? kGrassSunShare : kLeafSunShare)), 0.99), grass);
    }
    return o;
}
