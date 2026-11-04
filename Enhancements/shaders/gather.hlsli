// The gather kernel's work for one pixel in one dispatch: one sample of the light its hemisphere brings. A ray, the very
// one trace.hlsli's occlusion ray is but to any distance, finds the nearest surface, and where that surface faces the
// sun a second ray finds whether the sun reaches it. Shared by gather_cs.hlsl and, compiled as C++ through core/hlsl.h,
// by core/kernel.cpp, so the Mac's reference tracer gathers the very same samples; it keeps to what core/hlsl.h supports.
//
// A hit's light is the game's own where a scene face shows the hit: that face's HDR picture, which holds the game's sun,
// shadows and lights, and then no sun ray goes out. Elsewhere it is its triangle's albedo, from the light word
// core/light.h bakes into the triangle's spare word, times the sun's color by the cosine of its side where the second
// ray meets nothing, plus the game's ambient light for its side's normal. A miss reads the sky map. Nothing here runs a
// program at a hit.
//
// The includer defines, besides what trace_common.hlsli needs:
//   the Gather constants of core/trace.h's GatherConstants: float4 gather_sun, gather_shade, gather_ambient[4];
//     uint4 gather_counts
//   the GatherFaces constants of core/trace.h's GatherFaces: float4 face_rows[7][4], face_w[7]; uint4 face_info
//   the GatherLights constants of core/trace.h's GatherLights: float4 light_rows[16]; uint4 light_info
//   uint2 face_texel(uint face, uint2 pixel)   a face's lit picture at pixel: its HDR bits and its depth's bits
//   float3 load_normal(int2 pixel)     the G-buffer's world normal, decoded to -1..1 but not normalized
//   uint load_stencil(int2 pixel)
//   uint4 sky_row(uint index)          the sky map's texel index: its rgb as floats, and 1.0 for a real texel, 0.0 for a filled one
//   XYZ(v)                             a float4's first three components: v.xyz in HLSL, which vkd3d compiles to 8 fewer
//                                      temporaries and 42 fewer instructions here than three components put together
#include "trace_common.hlsli"
#include "local_light.hlsli"

// The sun ray from a hit takes its own sequence, by the sample's index; its choice between the sun and the traced lights
// takes its own hash.
static const uint kGatherSunSalt = 0x3C6EF372u;
// The most a light source gives off once brightened: more only adds speckle.
static const float kBrightestSource = 20.0f;
static const uint kGatherLightSalt = 0xA54FF53Au;
static const uint kGatherBounceSalt = 0x510E527Fu;
// The sky map's side: an octahedral map of kSkySide x kSkySide texels (core/light.h).
static const uint kSkySide = 128u;
// The gather kernel's counters, after the trace kernel's in the same buffer (core/trace.h's GatherCounter order): rays,
// guard stops, stack overflows, hits by where their light came from (core/light.h's AlbedoSource, then kFaceSource for a
// face's picture), and the sums of each sample's distance from its pixel's mean so far and of that mean, in luminance.
static const uint kGatherCounterCount = 10u;
static const uint kGatherSourceCounters = 3u;
static const uint kFaceSource = 4u;
static const uint kGatherDeviationCounter = 8u;
static const uint kGatherMeanCounter = 9u;
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

// One R11G11B10_FLOAT channel moved into a half float's bits, as core/light.h's hdr_rgb reads it: infinity as the
// channel's largest value, one step of its mantissa (step, in the half's bits) below, and NaN as nothing.
float half_of(uint h, uint step)
{
    return f16tof32(h > 0x7C00u ? 0u : min(h, 0x7C00u - step));
}

float3 hdr_rgb_of(uint bits)
{
    return float3(half_of((bits & 0x7FFu) << 4, 16u), half_of(((bits >> 11) & 0x7FFu) << 4, 16u), half_of((bits >> 22) << 5, 32u));
}

// The game's light at q from the first face that shows it: the face's HDR picture at q's pixel, where the face's depth
// puts the surface within max(0.05 m, 1%) of q's distance, as core/light.h's bake_albedo counts a direct sample. w is 1
// when a face shows q, else 0.
float4 face_light_of(float3 q)
{
    float4 p = float4(q.x, q.y, q.z, 1.0f);
    float width = float(face_info.y);
    float height = float(face_info.z);
    uint shown_by = 0u;
    uint bits = 0u;
    LOOP for (uint i = 0u; i < face_info.x; i++)
    {
        float w = dot(face_rows[i][3], p);
        float column = (dot(face_rows[i][0], p) / w * 0.5f + 0.5f) * width;
        float row = (0.5f - 0.5f * dot(face_rows[i][1], p) / w) * height;
        if (w > 1.0e-3f && column >= 0.0f && column < width && row >= 0.0f && row < height)
        {
            uint2 at = uint2(uint(column), uint(row));
            uint2 t = face_texel(i, at);
            float4 c = float4((float(at.x) + 0.5f) / width * 2.0f - 1.0f, 1.0f - (float(at.y) + 0.5f) / height * 2.0f, asfloat(t.y), 1.0f);
            float shown = 1.0f / dot(face_w[i], c);
            if (asfloat(t.y) > 0.0f && abs(shown - w) <= max(0.05f, 0.01f * w))
            {
                shown_by = 1u;
                bits = t.x;
                break;
            }
        }
    }
    float3 light = hdr_rgb_of(bits);
    return float4(light.x, light.y, light.z, float(shown_by));
}

struct GatherData
{
    float4 light;       // this sample's light; w: 1 from a hit, 2 from the sky
    float occlusion;    // trace.hlsli's occlusion sample of the same ray: (t / ao_distance)^2 of a hit within it, else 1
    uint world;         // 1 when the pixel is traced
    uint rays;
    uint guard;         // rays that met a cap of bvh.hlsli's
    uint overflow;      // rays whose stack overflowed
    uint lit;           // 1 when the sun reaches the hit
    uint source;        // the hit triangle's albedo source, bits 30 and 31 of its light word, or kFaceSource
};

// How much brighter a light source is than the picture shows it, as the game stores a window or a tube only a little brighter
// than what it lights: 1 up to source.y, and the whole source.x from twice that.
float source_gain_of(float seen, float2 source)
{
    if (!(source.x > 1.0f) || !(source.y > 0.0f))
        return 1.0f;
    float x = saturate((seen - source.y) / source.y);
    return 1.0f + (source.x - 1.0f) * x * x * (3.0f - 2.0f * x);
}

// The light along a ray from o in direction d, over up to bounces hits: the sky's on a miss; at a hit, a face's picture where
// one shows it, its light sources brighter by source; else its baked lights and, behind a second ray, the sun or a traced
// light. From a hit that is not the last, a cosine-weighted ray takes the next, times its albedo. At the last, the hit's
// ambient light stands in for all that would follow, so at the others it is left out, also of the picture where the picture
// holds it (gather_ambient[0].w of the hit's ambient). Every hit reflects surface times what it would, up to an albedo of
// 0.9 or its own if brighter: the game stores its albedos dark and lights them brightly, so its surfaces would bounce little light. Paths whose
// light has faded end at random, and the survivors count for those that ended, so that 15 bounces cost little more than the
// few that matter.
GatherData gather_ray_of(GatherData r, float3 o, float3 d, float3 eye, uint seed, uint index, float2 source, uint bounces, float surface, uint lane)
{
    float t_max = kSunReach;
    bool any_hit = false;
    float3 direct = float3(0.0f, 0.0f, 0.0f);   // the hit's sun light, added once the sun ray finds the sun
    float3 light = float3(0.0f, 0.0f, 0.0f);
    float3 through = float3(1.0f, 1.0f, 1.0f);  // what the hits so far let through
    float3 hn = float3(0.0f, 0.0f, 0.0f);       // the side of the last hit the next bounce leaves from
    float from = 2.0f;
    uint bounce = 0u;
    // One traversal in a loop over the sample's rays, so the shader holds one copy of it. Every pass but a sun ray's takes the
    // next bounce or ends.
    LOOP for (;;)
    {
        BvhHit h = trace_bvh(o, d, t_max, any_hit, trace_counts.z, lane);
        r.rays++;
        r.guard += h.guard;
        r.overflow += h.overflow;
        bool last = bounce + 1u >= bounces;
        uint mixed = seed ^ (index * 0x9E3779B9u) ^ (bounce * 0x85EBCA6Bu);
        if (any_hit)
        {
            if (h.prim == kNoHit)
            {
                light = light + direct;
                if (bounce == 0u)
                    r.lit = 1u;
            }
        }
        else
        {
            if (h.prim == kNoHit)
            {
                light = light + through * sky_at(d);
                break;
            }
            uint4 r0 = triangle_row(h.prim, 0u);
            uint4 r1 = triangle_row(h.prim, 1u);
            uint4 r2 = triangle_row(h.prim, 2u);
            float3 e1 = float3(asfloat(r1.x), asfloat(r1.y), asfloat(r1.z));
            float3 e2 = float3(asfloat(r2.x), asfloat(r2.y), asfloat(r2.z));
            float3 c = cross(e1, e2);
            // The side the ray met. The winding of a captured triangle gives the side the game draws.
            hn = c * (1.0f / max(length(c), 1.0e-20f));
            bool front = dot(hn, d) <= 0.0f;
            if (!front)
                hn = -hn;
            float3 q = o + d * h.t;
            o = q + hn * (0.02f + 1.0e-4f * length(q - eye));
            float3 albedo = albedo_of(r1.w);
            float gain = max(min(surface, 0.9f / max(max(albedo.x, max(albedo.y, albedo.z)), 1.0e-3f)), 1.0f);
            albedo = albedo * gain;
            if (bounce == 0u)
            {
                from = 1.0f;
                float near_share = min(h.t / trace_params.x, 1.0f);
                r.occlusion = near_share * near_share;
                r.source = r1.w >> 30;
            }
            // core/light.h keeps the rows from going below zero for any unit normal.
            float3 here = albedo * (XYZ(gather_ambient[0])
                + XYZ(gather_ambient[1]) * hn.x
                + XYZ(gather_ambient[2]) * hn.y
                + XYZ(gather_ambient[3]) * hn.z);
            float4 shown = face_light_of(q);
            if (shown.w > 0.5f)
            {
                float3 seen = XYZ(shown);
                float bright = source_gain_of(luminance_of(seen), source);
                if (bright > 1.0f)
                    seen = float3(min(seen.x * bright, kBrightestSource), min(seen.y * bright, kBrightestSource), min(seen.z * bright, kBrightestSource));
                seen = seen * gain;
                if (!last)
                {
                    here = seen - here * gather_ambient[0].w;
                    seen = float3(max(here.x, 0.0f), max(here.y, 0.0f), max(here.z, 0.0f));
                }
                light = light + through * seen;
                through = through * albedo;
                if (bounce == 0u)
                    r.source = kFaceSource;
            }
            else
            {
                if (!last)
                    here = float3(0.0f, 0.0f, 0.0f);
                // Vegetation and foliage take the sun on either side at the middle of the game's wrapped cosine, from the side
                // that faces it.
                uint cls = (r0.w >> 24) & 15u;
                bool card = cls == 1u || cls == 2u;
                // The game's own lights, baked on the triangle's front (core/trace.h): 16 is kBakedLightRange.
                if (front || card)
                    here = here + albedo_of(r2.w) * (16.0f * gain);
                light = light + through * here;
                through = through * albedo;
                float cosine = card ? 0.5f : dot(hn, XYZ(gather_shade));
                direct = cosine > 0.0f && sun_cone.w > 0.0f ? XYZ(gather_sun) * cosine : float3(0.0f, 0.0f, 0.0f);
                // The one shadow ray goes to the sun or to a traced light: one light, taken at random, weighed against the sun by
                // their light here unshadowed (the light's times the count of lights), and what the ray finds is divided by the
                // chance of its choice. Each light's share comes back in the mean, and one light's geometry is all a hit pays.
                float share = luminance_of(albedo * direct);
                // Hashed, not from the sample's sequence: its points along one lattice would tie the choice to where the gather
                // ray went, which biased two lights at two places to a fourth of their light.
                uint coin = hash32(mixed ^ kGatherLightSalt);
                float2 u = float2(float(coin >> 8), float(hash32(coin) >> 8)) * (1.0f / 16777216.0f);
                float count = float(light_info.x);
                uint k = min(uint(u.y * count), light_info.x - 1u);
                float geometry = light_info.x > 0u ? local_geometry_of(k, q, hn) * count : 0.0f;
                float weight = light_rows[k * 4u + 3u].y * geometry;
                float total = share + weight;
                if (total > 0.0f)
                {
                    t_max = kSunReach;
                    float3 toward = XYZ(sun_cone);
                    if (u.x * total >= share)
                    {
                        direct = XYZ(light_rows[k * 4u + 1u]) * geometry;
                        share = weight;
                        float3 v = XYZ(light_rows[k * 4u]) - q;
                        t_max = length(v);
                        toward = v * (1.0f / t_max);
                        t_max = max(t_max - 0.05f, 0.0f);
                    }
                    direct = through * direct * (total / max(share, 1.0e-20f));
                    // A card's next bounce leaves from the side the sun ray does.
                    if (card && dot(hn, toward) < 0.0f)
                    {
                        hn = -hn;
                        o = q + hn * (0.02f + 1.0e-4f * length(q - eye));
                    }
                    d = t_max < kSunReach ? toward : cone_direction_of(XYZ(sun_cone), sun_cone.w, sequence_point_of(seed, kGatherSunSalt + bounce, index));
                    any_hit = true;
                    continue;
                }
            }
        }
        if (last)
            break;
        // The next hit.
        if (bounce > 0u)
        {
            float keep = clamp(max(through.x, max(through.y, through.z)), 0.05f, 1.0f);
            if (float(hash32(mixed ^ kGatherBounceSalt) >> 8) * (1.0f / 16777216.0f) >= keep)
                break;
            through = through * (1.0f / keep);
        }
        bounce++;
        d = cosine_direction_of(hn, sequence_point_of(seed, kGatherBounceSalt + bounce, index));
        t_max = kSunReach;
        any_hit = false;
    }
    r.light = float4(light.x, light.y, light.z, from);
    return r;
}

// Sample index of a world pixel: the index-th ray of its occlusion sequence, cosine-weighted around the G-buffer's normal
// and mirrored above the surface, to the nearest surface at any distance; then, from a hit that no face shows and that
// the sun's shading direction lights, a ray in the sun's cone toward where the game's shadows put the sun.
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
    return gather_ray_of(r, o, d, eye, seed, index, float2(1.0f, 0.0f), max(gather_counts.y, 1u), max(gather_ambient[1].w, 1.0f), lane);
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
