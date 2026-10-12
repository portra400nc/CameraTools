// The trace kernel's work for one pixel in one dispatch: its position in the scene from the depth buffer, its normals,
// its rays and what they add to its history. Shared by trace_cs.hlsl and, compiled as C++ through core/hlsl.h, by
// core/kernel.cpp, so the Mac's reference tracer shoots the very same rays; it keeps to what core/hlsl.h supports.
//
// The includer defines, besides what trace_common.hlsli needs:
//   float3 load_normal(int2 pixel)     the G-buffer's world normal, decoded to -1..1 but not normalized
//   uint load_stencil(int2 pixel)
#include "trace_common.hlsli"

struct PixelTraceData
{
    float4 history;     // occlusion sum, sun sum, occlusion count, sun count
    uint world;         // 1 when the pixel is traced
    uint rays;
    uint guard;         // rays that met a cap of bvh.hlsli's
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
        else if (h.guard == 0u)
        {
            // A ray its caps cut short says nothing about the scene, so it is left out of the grid, which would otherwise
            // count it as meeting nothing and call the scene stale.
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
