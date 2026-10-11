// The trace kernel's shared HLSL compiled as C++ through hlsl.h: shaders/bvh.hlsli's traversal behind core/bvh.h's
// trace functions, and shaders/trace.hlsli's pixel behind core/trace.h's. The buffers and textures the shader loads from
// are a Bvh's and a TraceImage's vectors here, its constants a thread's copy of TraceConstants, and its groupshared
// stack a thread's own.
#include "bvh.h"
#include "hlsl.h"
#include "trace.h"

namespace photoreal::shader
{
    namespace
    {
        // The scene the current thread's rays traverse, and its stack.
        thread_local const Bvh *scene = nullptr;
        thread_local uint stack[24];

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

    namespace
    {
        float3 vec(const Vec3f &v) { return { v[0], v[1], v[2] }; }
        Vec3 vec(float3 v) { return { v.x, v.y, v.z }; }
        float4 row(const float v[4]) { return { v[0], v[1], v[2], v[3] }; }
        uint4 row(const uint32_t v[4]) { return { v[0], v[1], v[2], v[3] }; }

        BvhHit trace(const Bvh &bvh, const Vec3f &origin, const Vec3f &direction, float t_max, bool any_hit, uint32_t classes, TraversalStats *stats)
        {
            BvhHit hit {};
            hit.prim = kNoHit;
            if (bvh.nodes.empty())
                return hit;
            scene = &bvh;
            hit = trace_bvh(vec(origin), vec(direction), t_max, any_hit, classes, 0);
            scene = nullptr;
            if (stats)
                *stats = { hit.steps, hit.overflow != 0, hit.guard != 0 };
            return hit;
        }
    }
}

namespace photoreal
{
    std::optional<Hit> trace_nearest(const Bvh &bvh, const Vec3f &origin, const Vec3f &direction, float t_max, uint32_t classes, TraversalStats *stats)
    {
        const shader::BvhHit hit = shader::trace(bvh, origin, direction, t_max, false, classes, stats);
        if (hit.prim == shader::kNoHit)
            return std::nullopt;
        return Hit { hit.t, hit.prim, hit.u, hit.v };
    }

    bool trace_any(const Bvh &bvh, const Vec3f &origin, const Vec3f &direction, float t_max, uint32_t classes, TraversalStats *stats)
    {
        return shader::trace(bvh, origin, direction, t_max, true, classes, stats).prim != shader::kNoHit;
    }

    std::optional<Hit> brute_nearest(const Bvh &bvh, const Vec3f &origin, const Vec3f &direction, float t_max, uint32_t classes)
    {
        std::optional<Hit> best;
        float limit = t_max;
        shader::scene = &bvh;
        for (uint32_t i = 0; i < bvh.triangles.size(); i++)
        {
            const shader::BvhTriangleData tri = shader::load_bvh_triangle(i);
            if ((classes >> (tri.info >> 24 & 15u) & 1u) == 0)
                continue;
            const shader::float4 h = shader::intersect_triangle(tri, shader::vec(origin), shader::vec(direction), limit);
            if (h.x < limit)
            {
                limit = h.x;
                best = Hit { h.x, i, h.y, h.z };
            }
        }
        shader::scene = nullptr;
        return best;
    }


    PixelTrace trace_pixel(const Bvh &bvh, const TraceImage &frame, const TraceConstants &constants, uint32_t x, uint32_t y,
        const std::array<float, 4> &history)
    {
        using namespace shader;
        for (int i = 0; i < 4; i++)
            shader::clip_rows[i] = row(constants.clip_rows[i]);
        shader::eye_position = row(constants.eye_position);
        shader::sun_cone = row(constants.sun_cone);
        shader::trace_params = row(constants.trace_params);
        shader::trace_counts = row(constants.trace_counts);
        shader::trace_tiles = row(constants.trace_tiles);
        shader::trace_render = row(constants.trace_render);
        shader::image = &frame;
        shader::scene = &bvh;
        const PixelTraceData r = trace_pixel_of(int2(int(x), int(y)), float4(history[0], history[1], history[2], history[3]), 0);
        shader::scene = nullptr;
        shader::image = nullptr;
        PixelTrace out;
        out.history = { r.history.x, r.history.y, r.history.z, r.history.w };
        out.world = r.world != 0;
        out.counters = { r.rays, r.guard, r.overflow, r.tested, r.off };
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
