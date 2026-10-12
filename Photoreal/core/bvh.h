// The traced scene's bounding volume hierarchy: a binned-SAH BVH2 in the GPU's layout, and rays traced through it with
// shaders/bvh.hlsli compiled as C++, so the tests and the Mac's reference tracer run the compute shader's own traversal.
// Pure C++17 like trace_scene.h.
#pragma once

#include "trace_scene.h"

#include <cstdint>
#include <optional>
#include <vector>

namespace photoreal
{
    // 64 bytes, as shaders/bvh.hlsli loads it: both children's boxes, then their links. A link with bit 31 set is a leaf,
    // bits 0 to 3 its triangle count less 1 (1 to 16) and bits 4 to 30 its first triangle; kEmptyLink is no child, which
    // only the root of a scene of at most 16 triangles has; any other link is a node's index. A node's first internal
    // child is the next node, since the nodes are numbered depth first.
    struct BvhNode
    {
        float lo0[3], hi0[3], lo1[3], hi1[3];
        uint32_t link[2];
        uint32_t pad[2];
    };
    static_assert(sizeof(BvhNode) == 64);

    constexpr uint32_t kLeafBit = 0x80000000u;
    constexpr uint32_t kEmptyLink = 0xFFFFFFFFu;
    constexpr uint32_t kMaxLeaf = 16;
    constexpr uint32_t kMaxBvhTriangles = 1u << 27;

    // 48 bytes, as shaders/bvh.hlsli loads it: the first corner and the info word, then the two edges from it.
    struct BvhTriangle
    {
        float v0[3];
        uint32_t info;
        float e1[3];
        uint32_t pad0;
        float e2[3];
        uint32_t pad1;
    };
    static_assert(sizeof(BvhTriangle) == 48);

    struct Bvh
    {
        std::vector<BvhNode> nodes;             // the root first
        std::vector<BvhTriangle> triangles;     // in leaf order
        std::vector<uint32_t> source;           // each triangle's index in the build's input
        uint64_t hash = 0;                      // FNV-1a of the nodes' and then the triangles' bytes
        uint32_t leaves = 0;
        double sah = 0;                         // the surface area heuristic's cost, traversal and intersection costing 1 each
    };

    struct BvhOptions
    {
        uint32_t bins = 16;     // per axis, over the centroids' extent; 2 to 64
        uint32_t max_leaf = 4;  // a node of at most this many triangles is a leaf; up to kMaxLeaf when splitting costs more
        uint32_t tasks = 3;     // the first log2(tasks) + 2 levels build their left child on a thread of its own; the result does
                                // not depend on it
    };

    // Splits on the cheapest of bins - 1 planes per axis by the surface area heuristic, a node whose centroids coincide in
    // the middle. Empty triangles give an empty Bvh; more than kMaxBvhTriangles throws std::length_error, and an
    // allocation that fails std::bad_alloc.
    Bvh build_bvh(const std::vector<TraceTriangle> &triangles, const BvhOptions &options = {});

    // Classes as bits 1 << TraceClass, for the casters a ray sees.
    constexpr uint32_t kAllClasses = (1u << static_cast<uint32_t>(TraceClass::count)) - 1;
    // With the classes: a triangle with an alpha record counts only where its alpha test (core/alpha.h) keeps the hit.
    // Without this bit, or without an alpha buffer, every triangle is whole.
    constexpr uint32_t kAlphaTests = 0x80000000u;

    struct Hit
    {
        float t = 0;            // along the direction, which need not be unit length
        uint32_t triangle = 0;  // index into Bvh::triangles
        float u = 0, v = 0;     // barycentric coordinates of the hit: v0 + u e1 + v e2
    };

    // What one ray's traversal did: nodes visited, whether its 24-entry stack overflowed and a far child was left out,
    // whether it met a cap of shaders/bvh.hlsli's (its work, alpha programs or alpha instructions), its work (nodes,
    // triangles tested and alpha instructions), and the alpha programs it ran and their instructions.
    struct TraversalStats
    {
        uint32_t steps = 0;
        bool overflow = false;
        bool guard = false;
        uint32_t work = 0, alpha_tests = 0, alpha_steps = 0;
    };

    // The nearest hit and any hit beyond 1e-4 and before t_max, on triangles of the given classes, by the compute shader's
    // traversal, with the alpha tests of alpha's buffer when classes has kAlphaTests. Never throws.
    std::optional<Hit> trace_nearest(const Bvh &bvh, const Vec3f &origin, const Vec3f &direction, float t_max, uint32_t classes = kAllClasses,
        TraversalStats *stats = nullptr, const std::vector<uint32_t> *alpha = nullptr);
    bool trace_any(const Bvh &bvh, const Vec3f &origin, const Vec3f &direction, float t_max, uint32_t classes = kAllClasses,
        TraversalStats *stats = nullptr, const std::vector<uint32_t> *alpha = nullptr);

    // The nearest hit by testing every triangle with the same intersection and alpha test, for the tests.
    std::optional<Hit> brute_nearest(const Bvh &bvh, const Vec3f &origin, const Vec3f &direction, float t_max, uint32_t classes = kAllClasses,
        const std::vector<uint32_t> *alpha = nullptr);
}
