// The gather kernel's work for one pixel in one dispatch: one sample of the light its hemisphere brings. A ray, the very
// one trace.hlsli's occlusion ray is but to any distance, finds the nearest surface, and where that surface faces the
// sun a second ray finds whether the sun reaches it. Shared by gather_cs.hlsl and, compiled as C++ through core/hlsl.h,
// by core/kernel.cpp, so the Mac's reference tracer gathers the very same samples; it keeps to what core/hlsl.h supports.
//
// A hit's light is its triangle's albedo, from the light word core/light.h bakes into the triangle's spare word, times
// the sun's color by the cosine of its side where the second ray meets nothing, plus the game's ambient light for its
// side's normal. A miss reads the sky map. Nothing here fetches a texture or runs a program at a hit.
//
// The includer defines, besides what trace_common.hlsli needs:
//   the Gather constants of core/trace.h's GatherConstants: float4 gather_sun, gather_shade, gather_ambient[4];
//     uint4 gather_counts
//   float3 load_normal(int2 pixel)     the G-buffer's world normal, decoded to -1..1 but not normalized
//   uint load_stencil(int2 pixel)
//   uint4 sky_row(uint index)          the sky map's texel index: its rgb as floats, and 1.0 for a real texel, 0.0 for a filled one
//   XYZ(v)                             a float4's first three components: v.xyz in HLSL, which vkd3d compiles to 8 fewer
//                                      temporaries and 42 fewer instructions here than three components put together
#include "trace_common.hlsli"

// The sun ray from a hit takes its own sequence, by the sample's index.
static const uint kGatherSunSalt = 0x3C6EF372u;
// The sky map's side: an octahedral map of kSkySide x kSkySide texels (core/light.h).
static const uint kSkySide = 128u;
// The gather kernel's counters, after the trace kernel's in the same buffer (core/trace.h's GatherCounter order): rays,
// guard stops, stack overflows, hits by where their triangle's albedo came from (core/light.h's AlbedoSource), and the
// sums of each sample's distance from its pixel's mean so far and of that mean, in luminance.
static const uint kGatherCounterCount = 9u;
static const uint kGatherSourceCounters = 3u;
static const uint kGatherDeviationCounter = 7u;
static const uint kGatherMeanCounter = 8u;
// The noise sums count in 1 / 4096 (kNoiseScale) a pixel, capped at kNoiseCap, and a group adds them to the buffer in
// 1 / 16, so a dispatch of every pixel of a 3840x2160 frame stays under 2^32.
static const float kNoiseCap = 16.0f;
static const float kNoiseScale = 4096.0f;
static const uint kNoiseShift = 8u;

// A triangle's albedo from its light word: 10 bits a channel, each the square root of the linear value.
float3 albedo_of(uint w)
{
    float3 r = float3(float(w & 1023u), float((w >> 10) & 1023u), float((w >> 20) & 1023u)) * (1.0f / 1023.0f);
    return r * r;
}

// The sky map's texel a direction reads: the octahedron's upper half fills the diamond |u| + |v| <= 1 of the map, and
// its lower half is folded out into the four corners.
uint sky_texel_of(float3 d)
{
    float scale = 1.0f / (abs(d.x) + abs(d.y) + abs(d.z));
    float u = d.x * scale;
    float v = d.z * scale;
    if (d.y < 0.0f)
    {
        float fu = (1.0f - abs(v)) * (u >= 0.0f ? 1.0f : -1.0f);
        float fv = (1.0f - abs(u)) * (v >= 0.0f ? 1.0f : -1.0f);
        u = fu;
        v = fv;
    }
    uint x = min(uint(max(u * 0.5f + 0.5f, 0.0f) * float(kSkySide)), kSkySide - 1u);
    uint y = min(uint(max(v * 0.5f + 0.5f, 0.0f) * float(kSkySide)), kSkySide - 1u);
    return y * kSkySide + x;
}

float3 sky_at(float3 d)
{
    uint4 t = sky_row(sky_texel_of(d));
    return float3(asfloat(t.x), asfloat(t.y), asfloat(t.z));
}

float luminance_of(float3 c) { return c.x * 0.2126f + c.y * 0.7152f + c.z * 0.0722f; }

struct GatherData
{
    float4 light;       // this sample's light; w: 1 from a hit, 2 from the sky
    float occlusion;    // trace.hlsli's occlusion sample of the same ray: (t / ao_distance)^2 of a hit within it, else 1
    uint world;         // 1 when the pixel is traced
    uint rays;
    uint guard;         // rays that met a cap of bvh.hlsli's
    uint overflow;      // rays whose stack overflowed
    uint lit;           // 1 when the sun reaches the hit
    uint source;        // the hit triangle's albedo source, bits 30 and 31 of its light word
};

// Sample index of a world pixel: the index-th ray of its occlusion sequence, cosine-weighted around the G-buffer's normal
// and mirrored above the surface, to the nearest surface at any distance; then, from a hit that the sun's shading
// direction lights, a ray in the sun's cone toward where the game's shadows put the sun.
GatherData gather_pixel_of(int2 pixel, uint index, uint lane)
{
    GatherData r;
    r.light = float4(0.0f, 0.0f, 0.0f, 0.0f);
    r.occlusion = 1.0f;
    r.world = 0u;
    r.guard = 0u;
    r.overflow = 0u;
    r.rays = 0u;
    r.lit = 0u;
    r.source = 0u;
    uint s = load_stencil(pixel);
    float depth = load_depth(pixel);
    if (!is_world_stencil(s) || !(depth > 0.0f))
        return r;
    r.world = 1u;
    float3 eye = XYZ(eye_position);
    float3 p = scene_position(pixel, depth);
    float dist = length(p - eye);
    float3 view = (eye - p) / dist;
    float3 stored = load_normal(pixel);
    float stored_length = length(stored);
    float3 ng = geometric_normal(pixel, p, eye, stored_length > 1.0e-6f ? stored / stored_length : view);
    float3 n = stored_length > 1.0e-6f ? stored / stored_length : ng;
    uint seed = pixel_seed_of(pixel, trace_tiles.w);
    // The occlusion ray of trace.hlsli, sample for sample.
    float3 o = p + ng * (0.02f + 1.0e-4f * dist);
    float3 d = cosine_direction_of(n, sequence_point_of(seed, kAoSalt, index));
    float below = dot(d, ng);
    if (below < 0.0f)
        d = d - ng * (2.0f * below);
    float t_max = kSunReach;
    bool any_hit = false;
    float3 direct = float3(0.0f, 0.0f, 0.0f);   // the hit's sun light, added once the sun ray finds the sun
    float3 light = float3(0.0f, 0.0f, 0.0f);
    float from = 2.0f;
    // One traversal in a loop over the sample's rays, so the shader holds one copy of it: 0 the gather ray, 1 the sun ray.
    LOOP for (uint kind = 0u; kind < 2u; kind++)
    {
        BvhHit h = trace_bvh(o, d, t_max, any_hit, trace_counts.z, lane);
        r.rays++;
        r.guard += h.guard;
        r.overflow += h.overflow;
        if (kind == 1u)
        {
            if (h.prim == kNoHit)
            {
                light = light + direct;
                r.lit = 1u;
            }
            break;
        }
        if (h.prim == kNoHit)
        {
            light = sky_at(d);
            break;
        }
        from = 1.0f;
        float near_share = min(h.t / trace_params.x, 1.0f);
        r.occlusion = near_share * near_share;
        uint4 r0 = triangle_row(h.prim, 0u);
        uint4 r1 = triangle_row(h.prim, 1u);
        uint4 r2 = triangle_row(h.prim, 2u);
        float3 e1 = float3(asfloat(r1.x), asfloat(r1.y), asfloat(r1.z));
        float3 e2 = float3(asfloat(r2.x), asfloat(r2.y), asfloat(r2.z));
        float3 c = cross(e1, e2);
        // The side the ray met.
        float3 hn = c * (1.0f / max(length(c), 1.0e-20f));
        if (dot(hn, d) > 0.0f)
            hn = -hn;
        float3 albedo = albedo_of(r1.w);
        r.source = r1.w >> 30;
        // core/light.h keeps the rows from going below zero for any unit normal.
        light = albedo * (XYZ(gather_ambient[0])
            + XYZ(gather_ambient[1]) * hn.x
            + XYZ(gather_ambient[2]) * hn.y
            + XYZ(gather_ambient[3]) * hn.z);
        // Vegetation and foliage take the sun on either side at the middle of the game's wrapped cosine, from the side
        // that faces it.
        uint cls = (r0.w >> 24) & 15u;
        bool card = cls == 1u || cls == 2u;
        float cosine = card ? 0.5f : dot(hn, XYZ(gather_shade));
        if (!(cosine > 0.0f) || !(sun_cone.w > 0.0f))
            break;
        direct = albedo * XYZ(gather_sun) * cosine;
        float3 q = o + d * h.t;
        float side = card && dot(hn, XYZ(sun_cone)) < 0.0f ? -1.0f : 1.0f;
        o = q + hn * (side * (0.02f + 1.0e-4f * length(q - eye)));
        d = cone_direction_of(XYZ(sun_cone), sun_cone.w, sequence_point_of(seed, kGatherSunSalt, index));
        any_hit = true;
    }
    r.light = float4(light.x, light.y, light.z, from);
    return r;
}

// A pixel's sample in a dispatch: none once it has gather_counts.x of them, else the next of its sequence.
GatherData gather_next_of(int2 pixel, float4 history, uint lane)
{
    if (history.z < float(gather_counts.x))
        return gather_pixel_of(pixel, uint(history.z), lane);
    GatherData none;
    none.light = float4(0.0f, 0.0f, 0.0f, 0.0f);
    none.occlusion = 1.0f;
    none.world = 0u;
    none.rays = 0u;
    none.guard = 0u;
    none.overflow = 0u;
    none.lit = 0u;
    none.source = 0u;
    return none;
}

struct GatherSums
{
    float4 history;     // the trace's: occlusion sum, sun sum, their counts
    float4 hit;         // the hits' light summed, and the sun-lit hits
    float4 sky;         // the misses' sky summed, and the misses
    uint deviation;     // the sample's distance from the pixel's mean before it, and that mean, in luminance, in 1 / kNoiseScale
    uint mean;
};

// A world pixel's sample added to its sums. The occlusion count is the gather's sample count, so the trace kernel, which
// then shoots no occlusion ray, leaves it alone.
GatherSums gather_add_of(float4 history, float4 hit, float4 sky, GatherData r)
{
    GatherSums o;
    float lum = luminance_of(XYZ(r.light));
    // The first sample is its own mean.
    float mean = lum;
    if (history.z > 0.0f)
        mean = luminance_of(XYZ(hit) + XYZ(sky)) / history.z;
    o.deviation = uint(min(abs(lum - mean), kNoiseCap) * kNoiseScale);
    o.mean = uint(min(mean, kNoiseCap) * kNoiseScale);
    o.history = float4(history.x + r.occlusion, history.y, history.z + 1.0f, history.w);
    o.hit = hit;
    o.sky = sky;
    if (r.light.w < 1.5f)
        o.hit = hit + float4(r.light.x, r.light.y, r.light.z, float(r.lit));
    else
        o.sky = sky + float4(r.light.x, r.light.y, r.light.z, 1.0f);
    return o;
}
