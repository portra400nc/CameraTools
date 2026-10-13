// The constants of the gather's draws at the combine step: the trace's (core/trace.h's TraceConstants) at b1, as
// apply_trace_ps.hlsl reads them, and the apply's own at b2 (core/trace.h's GatherApplyConstants).

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

cbuffer GatherApply : register(b2)
{
    float4 gather_apply;        // strength, the filter's step in pixels, 0, 0
    uint4 gather_modes;         // ENHANCEMENTS_GATHER_SKY_*, the view (kGatherView*), 1 while the pixels have under 16 samples, 0
    float4 sky_harmonics[9];    // rgb: core/light.h's SkyMap::harmonics, the sky map's light on a surface of normal n over pi
};

static const uint kSkyOcclusion = 0u, kSkyVisibility = 1u, kSkyWeighted = 2u, kSkyMap = 3u;
static const uint kGatherViewNone = 0u, kGatherViewBounce = 1u, kGatherViewSkyVisibility = 2u, kGatherViewSkyMap = 3u;
