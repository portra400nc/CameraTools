// The gather's last draw at the combine step: the diffuse irradiance on world pixels with samples, from the gather's
// averages (gather_average_ps.hlsl, through gather_filter_ps.hlsl's passes when the filter is on), and the game's
// irradiance everywhere else, because the result is copied over the game's buffer whole. With W the sky's share, the
// pixel's own occlusion times the filtered sky's share over the filtered occlusion, so contact shadows keep their detail:
//   OCCLUSION:   level x game x lerp(1, own occlusion, ao_strength) + strength x B
//   VISIBILITY:  level x game x lerp(1, W, ao_strength) + strength x B
//   WEIGHTED:    as VISIBILITY, the share weighted by the sky's brightness
//   MAP:         level x K + strength x B
// While the pixels have under 16 samples the filter runs at least once and the own occlusion is the filtered one, as
// T1's apply draw blurs below 16. The shadow mask is apply_trace_ps.hlsl's, drawn before it.
#include "common.hlsli"
#include "gather.cbuffer.hlsli"

Texture2D<uint2> stencil : register(t3);            // mirror of the main depth; stencil in .g
StructuredBuffer<float4> history : register(t4);    // the trace's: occlusion sum, sun sum, their counts
Texture2D<float4> bounce_in : register(t5);         // B, f
Texture2D<float4> sky_in : register(t6);            // K, occlusion
Texture2D<float3> irradiance : register(t7);        // mirror of the game's diffuse irradiance

float3 main(float4 position : SV_Position) : SV_Target0
{
    int2 pixel = int2(position.xy);
    float3 game = irradiance.Load(int3(pixel, 0));
    float4 h = history[uint(pixel.y) * trace_render.x + uint(pixel.x)];
    if (!is_world(stencil.Load(int3(pixel, 0)).g) || !(h.z > 0))
        return game;
    float4 bounce = bounce_in.Load(int3(pixel, 0));
    float4 sky = sky_in.Load(int3(pixel, 0));
    float level = trace_params.w;
    float strength = gather_apply.x;
    float own = gather_modes.z != 0u ? sky.w : h.x / h.z;
    float3 lit = strength * bounce.rgb;
    if (gather_modes.x == kSkyMap)
        return level * sky.rgb + lit;
    float share = gather_modes.x == kSkyOcclusion ? own : own * bounce.w / max(sky.w, 1e-3);
    return game * (level * lerp(1, share, trace_params.y)) + lit;
}
