// The trace kernel's shared HLSL compiled as C++ through hlsl.h: shaders/bvh.hlsli's traversal behind core/bvh.h's
// trace functions, shaders/alpha_mask.hlsli's alpha test behind core/alpha_bake.h's alpha_mask_keeps, and
// shaders/trace.hlsli's pixel behind core/trace.h's. The buffers and textures the shader loads from are a Bvh's and a
// TraceImage's vectors here, its constants a thread's copy of TraceConstants, and its groupshared stack a thread's own.
#include "alpha_bake.h"
#include "bvh.h"
#include "hlsl.h"
#include "trace.h"

namespace photoreal::shader
{
    namespace
    {
        // The scene the current thread's rays traverse, its stack and its mask buffer (none: every triangle opaque).
        thread_local const Bvh *scene = nullptr;
        thread_local uint stack[24];
        thread_local const std::vector<uint32_t> *alpha = nullptr;

        uint alpha_word(uint index) { return alpha && index < alpha->size() ? (*alpha)[index] : 0u; }
        uint4 alpha_row(uint index) { return { alpha_word(index), alpha_word(index + 1), alpha_word(index + 2), alpha_word(index + 3) }; }

        uint4 node_row(uint node, uint row)
        {
            uint4 r;
            std::memcpy(&r, reinterpret_cast<const uint8_t *>(&scene->nodes[node]) + row * 16, sizeof r);
            return r;
        }

        uint4 triangle_row(uint index, uint row)
        {
            uint4 r;
            std::memcpy(&r, reinterpret_cast<const uint8_t *>(&scene->triangles[index]) + row * 16, sizeof r);
            return r;
        }

        // The frame and the constants the current thread's pixel is traced with, as the shader's cbuffer names them.
        thread_local const TraceImage *image = nullptr;
        thread_local float4 clip_rows[4];
        thread_local float4 eye_position, sun_cone, trace_params;
        thread_local uint4 trace_counts, trace_tiles, trace_render;

        size_t texel(int2 pixel) { return size_t(pixel.y) * image->size.width + size_t(pixel.x); }
        float load_depth(int2 pixel) { return image->depth[texel(pixel)]; }
        uint load_stencil(int2 pixel) { return image->stencil[texel(pixel)]; }
        // R10G10B10A2_UNORM as the SRV reads it, then * 2 - 1.
        float3 load_normal(int2 pixel)
        {
            const uint v = image->normals[texel(pixel)];
            const auto unorm = [](uint bits) { return float(bits & 1023u) / 1023.0f * 2.0f - 1.0f; };
            return { unorm(v), unorm(v >> 10), unorm(v >> 20) };
        }
    }

#define LOOP
#define STACK_STORE(lane, slot, value) ((void)(lane), stack[(slot)] = (value))
#define STACK_LOAD(lane, slot) ((void)(lane), stack[(slot)])
#include "../shaders/trace.hlsli"
#undef STACK_LOAD
#undef STACK_STORE
#undef LOOP

    static_assert(kStackSize == sizeof stack / sizeof stack[0] && kLeafLink == kLeafBit && kNoLink == kEmptyLink);
    static_assert(kOpaqueRecord == kOpaque && kAlphaTests == photoreal::kAlphaTests && kMaskHeaderWords == photoreal::kMaskHeaderWords
        && kMaskRecordWords == photoreal::kMaskRecordWords && kMaskSide == kAtlasTile);
    static_assert(kTraceCounterCount == kTraceCounters && kGridCounters == static_cast<uint32_t>(TraceCounter::grid) && kGridSlots == photoreal::kGridSlots
        && kGridNoHit == static_cast<uint32_t>(GridSlot::no_hit) && kGridNearer == static_cast<uint32_t>(GridSlot::nearer)
        && kGridFarther == static_cast<uint32_t>(GridSlot::farther));

    namespace
    {
        float3 vec(const Vec3f &v) { return { v[0], v[1], v[2] }; }
        Vec3 vec(float3 v) { return { v.x, v.y, v.z }; }
        float4 row(const float v[4]) { return { v[0], v[1], v[2], v[3] }; }
        uint4 row(const uint32_t v[4]) { return { v[0], v[1], v[2], v[3] }; }

        BvhHit trace(const Bvh &bvh, const Vec3f &origin, const Vec3f &direction, float t_max, bool any_hit, uint32_t classes, TraversalStats *stats,
            const std::vector<uint32_t> *alpha_buffer)
        {
            BvhHit hit {};
            hit.prim = kNoHit;
            if (bvh.nodes.empty())
                return hit;
            scene = &bvh;
            alpha = alpha_buffer;
            hit = trace_bvh(vec(origin), vec(direction), t_max, any_hit, alpha_buffer ? classes : classes & ~kAlphaTests, 0);
            alpha = nullptr;
            scene = nullptr;
            if (stats)
                *stats = { hit.steps, hit.overflow != 0, hit.guard != 0, hit.work, hit.alpha_tests };
            return hit;
        }
    }
}

namespace photoreal
{
    std::optional<Hit> trace_nearest(const Bvh &bvh, const Vec3f &origin, const Vec3f &direction, float t_max, uint32_t classes, TraversalStats *stats,
        const std::vector<uint32_t> *alpha)
    {
        const shader::BvhHit hit = shader::trace(bvh, origin, direction, t_max, false, classes, stats, alpha);
        if (hit.prim == shader::kNoHit)
            return std::nullopt;
        return Hit { hit.t, hit.prim, hit.u, hit.v };
    }

    bool trace_any(const Bvh &bvh, const Vec3f &origin, const Vec3f &direction, float t_max, uint32_t classes, TraversalStats *stats,
        const std::vector<uint32_t> *alpha)
    {
        return shader::trace(bvh, origin, direction, t_max, true, classes, stats, alpha).prim != shader::kNoHit;
    }

    std::optional<Hit> brute_nearest(const Bvh &bvh, const Vec3f &origin, const Vec3f &direction, float t_max, uint32_t classes,
        const std::function<bool(uint32_t, float, float)> &keeps)
    {
        std::optional<Hit> best;
        float limit = t_max;
        for (uint32_t i = 0; i < bvh.triangles.size(); i++)
        {
            shader::scene = &bvh;
            const shader::BvhTriangleData tri = shader::load_bvh_triangle(i);
            shader::scene = nullptr;
            if ((classes >> (tri.info >> 24 & 15u) & 1u) == 0)
                continue;
            const shader::float4 h = shader::intersect_triangle(tri, shader::vec(origin), shader::vec(direction), limit);
            if (h.x < limit && keeps && (classes & kAlphaTests) != 0 && (tri.info & kOpaque) != kOpaque && !keeps(tri.info & kOpaque, h.y, h.z))
                continue;
            if (h.x < limit)
            {
                limit = h.x;
                best = Hit { h.x, i, h.y, h.z };
            }
        }
        return best;
    }

    std::optional<Hit> brute_nearest(const Bvh &bvh, const Vec3f &origin, const Vec3f &direction, float t_max, uint32_t classes,
        const std::vector<uint32_t> *alpha)
    {
        if (!alpha)
            return brute_nearest(bvh, origin, direction, t_max, classes, std::function<bool(uint32_t, float, float)> {});
        return brute_nearest(bvh, origin, direction, t_max, classes,
            [alpha](uint32_t record, float u, float v) { return alpha_mask_keeps(*alpha, record, u, v); });
    }

    bool alpha_mask_keeps(const std::vector<uint32_t> &masks, uint32_t record, float u, float v)
    {
        shader::alpha = &masks;
        const bool keeps = shader::alpha_mask_keeps(record, u, v);
        shader::alpha = nullptr;
        return keeps;
    }


    PixelTrace trace_pixel(const Bvh &bvh, const TraceImage &frame, const TraceConstants &constants, uint32_t x, uint32_t y,
        const std::array<float, 4> &history, const std::vector<uint32_t> *alpha)
    {
        using namespace shader;
        for (int i = 0; i < 4; i++)
            shader::clip_rows[i] = row(constants.clip_rows[i]);
        shader::eye_position = row(constants.eye_position);
        shader::sun_cone = row(constants.sun_cone);
        shader::trace_params = row(constants.trace_params);
        shader::trace_counts = row(constants.trace_counts);
        if (!alpha)
            shader::trace_counts.z &= ~photoreal::kAlphaTests;
        shader::trace_tiles = row(constants.trace_tiles);
        shader::trace_render = row(constants.trace_render);
        shader::image = &frame;
        shader::scene = &bvh;
        shader::alpha = alpha;
        const PixelTraceData r = trace_pixel_of(int2(int(x), int(y)), float4(history[0], history[1], history[2], history[3]), 0);
        shader::alpha = nullptr;
        shader::scene = nullptr;
        shader::image = nullptr;
        PixelTrace out;
        out.history = { r.history.x, r.history.y, r.history.z, r.history.w };
        out.world = r.world != 0;
        out.counters = { r.rays, r.guard, r.overflow, r.tested, r.off };
        if (r.tested != 0)
        {
            const auto cls = static_cast<GridClass>(r.grid_class);
            out.counters[grid_counter(cls, GridSlot::tested)] = 1;
            if (r.off != 0)
                out.counters[grid_counter(cls, static_cast<GridSlot>(r.grid_reason))] = 1;
        }
        return out;
    }

    uint32_t pixel_seed(uint32_t x, uint32_t y, uint32_t generation) { return shader::pixel_seed_of(shader::int2(int(x), int(y)), generation); }

    std::array<float, 2> sequence_point(uint32_t seed, uint32_t salt, uint32_t index)
    {
        const shader::float2 p = shader::sequence_point_of(seed, salt == 0 ? shader::kAoSalt : shader::kSunSalt, index);
        return { p.x, p.y };
    }

    Vec3 cosine_direction(const Vec3 &normal, const std::array<float, 2> &u)
    {
        return shader::vec(shader::cosine_direction_of({ normal[0], normal[1], normal[2] }, { u[0], u[1] }));
    }

    Vec3 cone_direction(const Vec3 &axis, float cos_max, const std::array<float, 2> &u)
    {
        return shader::vec(shader::cone_direction_of({ axis[0], axis[1], axis[2] }, cos_max, { u[0], u[1] }));
    }
}
