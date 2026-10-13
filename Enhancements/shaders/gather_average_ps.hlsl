// The gather's first draw at the combine step: each world pixel's averages from its sums, for the filter and the apply
// draw. Target 0 is the bounce B and the far term f, the share of rays that met the sky, or with the WEIGHTED sky that
// share weighted by how bright the sky it met is against the sky map's own light on the pixel's normal; target 1 is the
// sky the rays met K and the occlusion. Pixels that are not world or have no sample get zeros, which the filter's
// weights leave out. Target 2, bound only for a gather view, gets the bounce, the sky visibility (blue without a sample)
// or the sky map stretched over the screen, its filled texels at half.
#include "gbuffer.hlsli"
#include "gather.cbuffer.hlsli"

StructuredBuffer<float4> history : register(t0);    // the trace's: occlusion sum, sun sum, their counts
StructuredBuffer<float4> gather_hit : register(t5); // the hits' light summed, and the sun-lit hits
StructuredBuffer<float4> gather_sky : register(t6); // the misses' sky summed, and the misses
ByteAddressBuffer sky_map : register(t7);           // core/light.h's sky map: 128 x 128 float4 texels

static const uint kSkySide = 128u;

struct Targets
{
    float4 bounce : SV_Target0;     // B, f
    float4 sky : SV_Target1;        // K, occlusion
    float4 view : SV_Target2;
};

float luminance_of(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }

// core/light.h's sky_irradiance: the harmonics' polynomial at a unit normal.
float3 sky_light(float3 n)
{
    return sky_harmonics[0].rgb + sky_harmonics[1].rgb * n.y + sky_harmonics[2].rgb * n.z + sky_harmonics[3].rgb * n.x + sky_harmonics[4].rgb * (n.x * n.y)
        + sky_harmonics[5].rgb * (n.y * n.z) + sky_harmonics[6].rgb * (3 * n.z * n.z - 1) + sky_harmonics[7].rgb * (n.x * n.z)
        + sky_harmonics[8].rgb * (n.x * n.x - n.y * n.y);
}

Targets main(float4 position : SV_Position)
{
    int2 pixel = int2(position.xy);
    uint at = uint(pixel.y) * trace_render.x + uint(pixel.x);
    Targets o;
    o.bounce = 0;
    o.sky = 0;
    o.view = 0;
    if (gather_modes.y == kGatherViewSkyMap)
    {
        uint2 texel = min(uint2(pixel * kSkySide / int2(trace_render.xy)), kSkySide - 1);
        float4 t = asfloat(sky_map.Load4((texel.y * kSkySide + texel.x) * 16u));
        o.view = float4(t.rgb * (t.w > 0 ? 1 : 0.5), 1);
    }
    float4 h = history[at];
    if (!is_world(stencil.Load(int3(pixel, 0)).g) || !(h.z > 0))
    {
        if (gather_modes.y == kGatherViewSkyVisibility)
            o.view = float4(0, 0, 1, 1);
        return o;
    }
    float4 hit = gather_hit[at];
    float4 sky = gather_sky[at];
    float n = h.z;
    float3 bounce = hit.rgb / n;
    float3 sky_seen = sky.rgb / n;
    float vis = sky.w / n;
    float far = vis;
    if (gather_modes.x == kSkyWeighted)
        far = luminance_of(sky_seen) / max(luminance_of(sky_light(normalize(normals.Load(int3(pixel, 0)).xyz * 2 - 1))), 1e-4);
    o.bounce = float4(bounce, far);
    o.sky = float4(sky_seen, h.x / n);
    if (gather_modes.y == kGatherViewBounce)
        o.view = float4(bounce, 1);
    else if (gather_modes.y == kGatherViewSkyVisibility)
        o.view = float4(vis.xxx, 1);
    return o;
}
