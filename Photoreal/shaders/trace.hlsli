// The trace kernel's work for one pixel in one dispatch: its position in the scene from the depth buffer, its normals,
// its rays and what they add to its history. Shared by trace_cs.hlsl and, compiled as C++ through core/hlsl.h, by
// core/kernel.cpp, so the Mac's reference tracer shoots the very same rays; it keeps to what core/hlsl.h supports.
//
// The includer defines, besides what bvh.hlsli needs:
//   the Trace constants of core/trace.h's TraceConstants: float4 clip_rows[4], eye_position, sun_cone, trace_params;
//     uint4 trace_counts, trace_tiles, trace_render
//   float load_depth(int2 pixel)       the stored reversed depth, 0 for the sky
//   float3 load_normal(int2 pixel)     the G-buffer's world normal, decoded to -1..1 but not normalized
//   uint load_stencil(int2 pixel)
#include "bvh.hlsli"

static const uint kAoSalt = 0x68E31DA4u;
static const uint kSunSalt = 0xB5297A4Du;
// Sun rays look this far: past everything a scene capture holds.
static const float kSunReach = 1.0e5f;
// The classes a ray from the camera checks the scene against: world and other, never cards nor characters.
static const uint kSolidClasses = 9u;
// The kernel's counters, core/trace.h's TraceCounter order: rays, guard stops, stack overflows, grid rays and those off,
// then for each grid class (stencil 128, 144, any other world stencil) its grid rays and those off by reason: met
// nothing, met the scene nearer than the depth buffer, or farther.
static const uint kGridCounters = 5u;
static const uint kGridSlots = 4u;
static const uint kTraceCounterCount = kGridCounters + 3u * kGridSlots;
static const uint kGridOn = 0u, kGridNoHit = 1u, kGridNearer = 2u, kGridFarther = 3u;

// lowbias32, Chris Wellons' integer hash.
uint hash32(uint x)
{
    x ^= x >> 16;
    x *= 0x7FEB352Du;
    x ^= x >> 15;
    x *= 0x846CA68Bu;
    x ^= x >> 16;
    return x;
}

uint pixel_seed_of(int2 pixel, uint generation)
{
    return hash32(uint(pixel.x) + hash32(uint(pixel.y) + hash32(generation)));
}

// The index-th point of the R2 sequence (Roberts' plastic-number lattice), turned by a rotation of the seed, all in
// 32-bit integers, so the GPU and the C++ reference agree to the bit. 3242174889 and 2447445414 are 2^32 / g and
// 2^32 / g^2 for g = 1.3247179572.
float2 sequence_point_of(uint seed, uint salt, uint index)
{
    uint rx = hash32(seed ^ salt);
    uint ry = hash32(rx + 0x9E3779B9u);
    uint a = index * 3242174889u + rx;
    uint b = index * 2447445414u + ry;
    return float2(float(a >> 8), float(b >> 8)) * (1.0f / 16777216.0f);
}

struct Basis
{
    float3 t;
    float3 b;
};

// Two tangents of a unit normal (Duff et al. 2017), with no singularity.
Basis basis_of(float3 n)
{
    float s = n.z >= 0.0f ? 1.0f : -1.0f;
    float a = -1.0f / (s + n.z);
    float b = n.x * n.y * a;
    Basis o;
    o.t = float3(1.0f + s * n.x * n.x * a, s * b, -s * n.x);
    o.b = float3(b, s + n.y * n.y * a, -n.y);
    return o;
}

float3 cosine_direction_of(float3 n, float2 u)
{
    float r = sqrt(u.x);
    float phi = 6.28318531f * u.y;
    Basis o = basis_of(n);
    return normalize(o.t * (r * cos(phi)) + o.b * (r * sin(phi)) + n * sqrt(max(0.0f, 1.0f - u.x)));
}

// Uniform over the solid angle of the cone.
float3 cone_direction_of(float3 axis, float cos_max, float2 u)
{
    float c = 1.0f - u.x * (1.0f - cos_max);
    float s = sqrt(max(0.0f, 1.0f - c * c));
    float phi = 6.28318531f * u.y;
    Basis o = basis_of(axis);
    return normalize(o.t * (s * cos(phi)) + o.b * (s * sin(phi)) + axis * c);
}

bool is_world_stencil(uint s) { return (s & 0x84u) == 0x80u; }

// Grass, vegetation and foliage, whose alpha-tested depth the scene's full cards do not match.
bool is_alpha_stencil(uint s) { return s == 129u || s == 136u || s == 137u; }

// A stored pixel's surface in the scene's frame. The stored row runs upside down when trace_render.z is set, as
// common.hlsli's stored_uv has it.
float3 scene_position(int2 pixel, float depth)
{
    float u = (float(pixel.x) + 0.5f) / float(trace_render.x);
    float v = (float(pixel.y) + 0.5f) / float(trace_render.y);
    if (trace_render.z != 0u)
        v = 1.0f - v;
    float4 clip = float4(u * 2.0f - 1.0f, 1.0f - v * 2.0f, depth, 1.0f);
    float w = dot(clip_rows[3], clip);
    return float3(dot(clip_rows[0], clip), dot(clip_rows[1], clip), dot(clip_rows[2], clip)) / w;
}

struct Neighbor
{
    float3 p;
    uint valid;
};

Neighbor neighbor_at(int2 q)
{
    Neighbor n;
    n.p = float3(0.0f, 0.0f, 0.0f);
    n.valid = 0u;
    if (q.x < 0 || q.y < 0 || q.x >= int(trace_render.x) || q.y >= int(trace_render.y))
        return n;
    float d = load_depth(q);
    if (!(d > 0.0f))
        return n;
    n.p = scene_position(q, d);
    n.valid = 1u;
    return n;
}

// The edge from p along one axis, to the nearer of its two neighbors with depth, so a silhouette does not tilt it.
float4 edge_of(float3 p, Neighbor after, Neighbor before)
{
    float la = after.valid != 0u ? length(after.p - p) : kMiss;
    float lb = before.valid != 0u ? length(p - before.p) : kMiss;
    if (after.valid == 0u && before.valid == 0u)
        return float4(0.0f, 0.0f, 0.0f, 0.0f);
    float3 e = la <= lb ? after.p - p : p - before.p;
    return float4(e.x, e.y, e.z, 1.0f);
}

// The surface's own normal from the depth buffer, facing the camera; fallback where the neighbors cannot give one.
float3 geometric_normal(int2 pixel, float3 p, float3 eye, float3 fallback)
{
    float4 ex = edge_of(p, neighbor_at(int2(pixel.x + 1, pixel.y)), neighbor_at(int2(pixel.x - 1, pixel.y)));
    float4 ey = edge_of(p, neighbor_at(int2(pixel.x, pixel.y + 1)), neighbor_at(int2(pixel.x, pixel.y - 1)));
    float3 n = fallback;
    if (ex.w > 0.0f && ey.w > 0.0f)
    {
        float3 c = cross(float3(ex.x, ex.y, ex.z), float3(ey.x, ey.y, ey.z));
        float l = length(c);
        if (l > 1.0e-12f)
            n = c / l;
    }
    return dot(n, eye - p) < 0.0f ? -n : n;
}

struct PixelTraceData
{
    float4 history;     // occlusion sum, sun sum, occlusion count, sun count
    uint world;         // 1 when the pixel is traced
    uint rays;
    uint guard;         // rays stopped by the step guard
    uint overflow;      // rays whose stack overflowed
    uint tested;        // 1 when its ray from the camera checked the scene
    uint off;           // 1 when that ray met the scene off the depth buffer, or not at all
    uint grid_class;    // the tested pixel's: 0 stencil 128, 1 stencil 144, 2 any other world stencil
    uint grid_reason;   // kGridOn, or why it is off: kGridNoHit, kGridNearer, kGridFarther
};

uint grid_class_of(uint s) { return s == 128u ? 0u : s == 144u ? 1u : 2u; }

PixelTraceData trace_pixel_of(int2 pixel, float4 history, uint lane)
{
    PixelTraceData r;
    r.history = history;
    r.world = 0u;
    r.rays = 0u;
    r.guard = 0u;
    r.overflow = 0u;
    r.tested = 0u;
    r.off = 0u;
    r.grid_class = 0u;
    r.grid_reason = kGridOn;
    uint s = load_stencil(pixel);
    float depth = load_depth(pixel);
    if (!is_world_stencil(s) || !(depth > 0.0f))
        return r;
    r.world = 1u;
    float3 eye = float3(eye_position.x, eye_position.y, eye_position.z);
    float3 p = scene_position(pixel, depth);
    float dist = length(p - eye);
    float3 view = (eye - p) / dist;
    float3 stored = load_normal(pixel);
    float stored_length = length(stored);
    float3 ng = geometric_normal(pixel, p, eye, stored_length > 1.0e-6f ? stored / stored_length : view);
    float3 n = stored_length > 1.0e-6f ? stored / stored_length : ng;
    // Off the surface along its own normal, further with distance, as depth precision falls.
    float3 origin = p + ng * (0.02f + 1.0e-4f * dist);
    uint seed = pixel_seed_of(pixel, trace_tiles.w);
    uint mask = trace_counts.z;

    // One traversal in a loop over the pixel's rays, so the shader holds one copy of it: 0 occlusion, 1 sun, 2 the
    // consistency grid's ray from the camera.
    bool want_ao = r.history.z < float(trace_counts.x);
    bool want_sun = r.history.w < float(trace_counts.y);
    bool want_check = trace_render.w != 0u && pixel.x % int(trace_render.w) == 0 && pixel.y % int(trace_render.w) == 0 && !is_alpha_stencil(s);
    LOOP for (uint kind = 0u; kind < 3u; kind++)
    {
        float3 o = origin;
        float3 d = view;
        float t_max = 0.0f;
        bool any_hit = false;
        uint classes = mask;
        if (kind == 0u)
        {
            if (!want_ao)
                continue;
            // Cosine-weighted around the G-buffer's normal, and mirrored above the surface where that normal leans past it.
            d = cosine_direction_of(n, sequence_point_of(seed, kAoSalt, uint(r.history.z)));
            float below = dot(d, ng);
            if (below < 0.0f)
                d = d - ng * (2.0f * below);
            t_max = trace_params.x;
        }
        else if (kind == 1u)
        {
            if (!want_sun)
                continue;
            if (!(sun_cone.w > 0.0f))
            {
                // No sun: the sample counts as lit, untraced.
                r.history.y += 1.0f;
                r.history.w += 1.0f;
                continue;
            }
            d = cone_direction_of(float3(sun_cone.x, sun_cone.y, sun_cone.z), sun_cone.w, sequence_point_of(seed, kSunSalt, uint(r.history.w)));
            t_max = kSunReach;
            any_hit = true;
        }
        else
        {
            if (!want_check)
                continue;
            o = eye;
            d = -view;
            t_max = dist * 1.5f + 1.0f;
            classes = kSolidClasses | (mask & kAlphaTests);
        }
        BvhHit h = trace_bvh(o, d, t_max, any_hit, classes, lane);
        r.rays++;
        r.guard += h.guard;
        r.overflow += h.overflow;
        if (kind == 0u)
        {
            float near_share = h.t / t_max;
            r.history.x += h.prim != kNoHit ? near_share * near_share : 1.0f;
            r.history.z += 1.0f;
        }
        else if (kind == 1u)
        {
            r.history.y += h.prim != kNoHit ? 0.0f : 1.0f;
            r.history.w += 1.0f;
        }
        else
        {
            r.tested = 1u;
            r.grid_class = grid_class_of(s);
            if (h.prim == kNoHit)
                r.grid_reason = kGridNoHit;
            else if (abs(h.t - dist) > max(0.05f, 0.01f * dist))
                r.grid_reason = h.t < dist ? kGridNearer : kGridFarther;
            r.off = r.grid_reason != kGridOn ? 1u : 0u;
        }
    }
    return r;
}
