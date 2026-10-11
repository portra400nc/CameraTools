// The trace kernel's shared HLSL compiled as C++ through hlsl.h: shaders/bvh.hlsli's traversal behind core/bvh.h's
// trace functions. The buffers the shader loads from are a Bvh's vectors here, and its groupshared stack a thread's own.
#include "bvh.h"
#include "hlsl.h"

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
    }

#define LOOP
#define STACK_STORE(lane, slot, value) ((void)(lane), stack[(slot)] = (value))
#define STACK_LOAD(lane, slot) ((void)(lane), stack[(slot)])
#include "../shaders/bvh.hlsli"
#undef STACK_LOAD
#undef STACK_STORE
#undef LOOP

    static_assert(kStackSize == sizeof stack / sizeof stack[0] && kLeafLink == kLeafBit && kNoLink == kEmptyLink);

    namespace
    {
        float3 vec(const Vec3f &v) { return { v[0], v[1], v[2] }; }

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
}
