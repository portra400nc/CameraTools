// The BVH: its shape on the deduplicated fixture (fixtures/scene-194351-near.bin through SceneBuilder), the same for any
// number of build threads; the compute shader's traversal, compiled as C++, against a test of every triangle for rays
// from the fixture's surfaces; and the edge cases: a ray through an edge two triangles share, a ray in a triangle's plane,
// a triangle reduced to a point, a scene of one triangle, an empty scene, and classes left out.
#include "../core/bvh.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

using namespace photoreal;

namespace
{
    int failures = 0;

    void check(const char *name, bool ok)
    {
        std::printf("%s %s\n", ok ? "PASS" : "FAIL", name);
        failures += ok ? 0 : 1;
    }

    // The fixture's triangles deduplicated as the add-on keeps them, as trace_scene_test.cpp reads them.
    std::vector<TraceTriangle> fixture_scene()
    {
        std::ifstream file("fixtures/scene-194351-near.bin", std::ios::binary);
        const std::string bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        if (bytes.size() < 8)
            throw std::runtime_error("missing fixtures/scene-194351-near.bin");
        uint32_t count = 0;
        std::memcpy(&count, bytes.data() + 4, 4);
        static const uint32_t kStencils[] = { 0, 128, 129, 133, 136, 137, 144, 165, 200 };
        SceneBuilder builder;
        for (uint32_t i = 0; i < count; i++)
        {
            const char *at = bytes.data() + 8 + size_t(i) * 40;
            float f[9];
            std::memcpy(f, at, sizeof f);
            SceneDrawRecord r;
            const uint8_t kind = static_cast<uint8_t>(at[37]), cls = static_cast<uint8_t>(at[38]);
            r.stencil = kind != 0;
            r.stencil_ref = kStencils[cls];
            r.depth_func = kind == 1 ? 3 : 7;
            r.depth_write = kind != 1;
            builder.add_draw(r, { { Vec3d { f[0], f[1], f[2] }, Vec3d { f[3], f[4], f[5] }, Vec3d { f[6], f[7], f[8] } } });
        }
        return builder.take(UINT64_MAX);
    }

    // xorshift32, so the rays are the same on every standard library.
    struct Random
    {
        uint32_t state = 2463534242u;
        float next()
        {
            state ^= state << 13;
            state ^= state >> 17;
            state ^= state << 5;
            return (state >> 8) * (1.0f / 16777216.0f);
        }
    };

    Vec3f operator*(const Vec3f &a, float s) { return { a[0] * s, a[1] * s, a[2] * s }; }
    Vec3f operator+(const Vec3f &a, const Vec3f &b) { return { a[0] + b[0], a[1] + b[1], a[2] + b[2] }; }
    Vec3f operator-(const Vec3f &a, const Vec3f &b) { return { a[0] - b[0], a[1] - b[1], a[2] - b[2] }; }
    Vec3f cross(const Vec3f &a, const Vec3f &b) { return { a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0] }; }
    Vec3f unit(const Vec3f &a) { return a * (1.0f / std::sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2])); }

    // The traversal agrees with every-triangle testing: no hit for both, or the same triangle, or the same distance
    // within 1e-6 of it (two triangles the ray meets at one point).
    bool agree(const std::optional<Hit> &a, const std::optional<Hit> &b)
    {
        if (!a || !b)
            return !a && !b;
        return a->triangle == b->triangle || std::fabs(a->t - b->t) <= 1e-6f * b->t;
    }

    TraceTriangle triangle(Vec3f a, Vec3f b, Vec3f c, TraceClass cls = TraceClass::world)
    {
        TraceTriangle t;
        t.corner = { a, b, c };
        t.info = triangle_info(cls);
        return t;
    }
}

int main()
{
    const std::vector<TraceTriangle> scene = fixture_scene();
    BvhOptions one;
    one.tasks = 1;
    const Bvh bvh = build_bvh(scene, one);
    {
        BvhOptions three, eight;
        three.tasks = 3;
        eight.tasks = 8;
        const Bvh b3 = build_bvh(scene, three), b8 = build_bvh(scene, eight);
        std::printf("INFO fixture BVH: %zu triangles, %zu nodes, %u leaves, SAH cost %.3f, hash %016llx\n", bvh.triangles.size(), bvh.nodes.size(), bvh.leaves,
            bvh.sah, static_cast<unsigned long long>(bvh.hash));
        check("the fixture's BVH has its literal shape and hash", bvh.triangles.size() == 14165 && bvh.nodes.size() == 4621 && bvh.leaves == 4622
            && bvh.hash == 0x5acc3b0243e8c541ull);
        check("3 and 8 build threads give the same nodes and triangles as 1", b3.hash == bvh.hash && b8.hash == bvh.hash && b3.nodes.size() == bvh.nodes.size()
            && std::memcmp(b8.nodes.data(), bvh.nodes.data(), bvh.nodes.size() * sizeof(BvhNode)) == 0 && b8.source == bvh.source);
        bool leaves_ok = true, order_ok = true;
        uint32_t covered = 0;
        for (const BvhNode &node : bvh.nodes)
            for (uint32_t link : node.link)
            {
                if ((link & kLeafBit) == 0)
                    order_ok &= link < bvh.nodes.size();
                else if (link != kEmptyLink)
                {
                    leaves_ok &= (link & 15u) + 1 <= kMaxLeaf;
                    covered += (link & 15u) + 1;
                }
            }
        check("every leaf holds 1 to 16 triangles, every triangle is in one leaf, every link names a node", leaves_ok && order_ok && covered == bvh.triangles.size());
        bool left_next = true;
        for (size_t i = 0; i < bvh.nodes.size(); i++)
            if ((bvh.nodes[i].link[0] & kLeafBit) == 0)
                left_next &= bvh.nodes[i].link[0] == i + 1;
        check("the nodes are numbered depth first: an internal left child is the next node", left_next);
    }
    {
        // From random points on the fixture's triangles, just off them: cosine directions around their normals within 2 m
        // and 10 m, and toward the sun to the end of the scene.
        Random random;
        const Vec3f sun = unit({ 0.4299f, 1.0f, 1.3619f });
        uint32_t rays = 0, agreed = 0, any_agreed = 0, hits = 0;
        uint64_t steps = 0;
        for (int i = 0; i < 1000; i++)
        {
            const TraceTriangle &t = scene[static_cast<size_t>(random.next() * scene.size()) % scene.size()];
            float u = random.next(), v = random.next();
            if (u + v > 1)
            {
                u = 1 - u;
                v = 1 - v;
            }
            Vec3f normal = unit(cross(t.corner[1] - t.corner[0], t.corner[2] - t.corner[0]));
            if (random.next() < 0.5f)
                normal = normal * -1.0f;
            const Vec3f origin = t.corner[0] + (t.corner[1] - t.corner[0]) * u + (t.corner[2] - t.corner[0]) * v + normal * 1e-3f;
            const Vec3f side = unit(cross(std::fabs(normal[1]) < 0.9f ? Vec3f { 0, 1, 0 } : Vec3f { 1, 0, 0 }, normal)), up = cross(normal, side);
            const float r = std::sqrt(random.next()), phi = 6.2831853f * random.next();
            const Vec3f cosine = unit(side * (r * std::cos(phi)) + up * (r * std::sin(phi)) + normal * std::sqrt(1 - r * r));
            const struct { Vec3f d; float t_max; } cases[] = { { cosine, 2.0f }, { cosine, 10.0f }, { sun, 1e5f } };
            for (const auto &c : cases)
            {
                TraversalStats stats;
                const std::optional<Hit> nearest = trace_nearest(bvh, origin, c.d, c.t_max, kAllClasses, &stats);
                const std::optional<Hit> brute = brute_nearest(bvh, origin, c.d, c.t_max);
                rays++;
                hits += brute.has_value();
                agreed += agree(nearest, brute);
                any_agreed += trace_any(bvh, origin, c.d, c.t_max) == brute.has_value();
                steps += stats.steps;
            }
        }
        std::printf("INFO %u rays from the fixture's surfaces: %u hit, %.1f nodes a ray\n", rays, hits, double(steps) / rays);
        check("3,000 rays: the traversal's nearest hit is every-triangle testing's", agreed == rays && hits > rays / 4);
        check("and its any hit says whether there is one", any_agreed == rays);
    }
    {
        // Two triangles of one square, and a ray straight down through the middle of the diagonal they share.
        const std::vector<TraceTriangle> square = {
            triangle({ 0, 0, 0 }, { 1, 0, 0 }, { 1, 0, 1 }), triangle({ 0, 0, 0 }, { 1, 0, 1 }, { 0, 0, 1 }), triangle({ 5, 0, 0 }, { 6, 0, 0 }, { 5, 1, 0 }),
            triangle({ 5, 2, 0 }, { 6, 2, 0 }, { 5, 3, 0 }), triangle({ 9, 0, 0 }, { 9, 0, 1 }, { 9, 1, 0 }), triangle({ 3, 3, 3 }, { 3, 3, 3 }, { 3, 3, 3 }),
        };
        const Bvh b = build_bvh(square, one);
        const std::optional<Hit> edge = trace_nearest(b, { 0.5f, 1, 0.5f }, { 0, -1, 0 }, 10);
        check("a ray through the edge two triangles share hits one of them", edge && std::fabs(edge->t - 1) < 1e-6f && agree(edge, brute_nearest(b, { 0.5f, 1, 0.5f }, { 0, -1, 0 }, 10)));
        check("a ray in a triangle's plane misses it", !trace_nearest(b, { -1, 0, 0.5f }, { 1, 0, 0 }, 3) && !brute_nearest(b, { -1, 0, 0.5f }, { 1, 0, 0 }, 3));
        check("a triangle reduced to a point is never hit", !trace_any(b, { 3, 5, 3 }, { 0, -1, 0 }, 10) && !trace_any(b, { 3, 3, 0 }, { 0, 0, 1 }, 10));
        check("a hit past t_max is no hit", !trace_any(b, { 0.5f, 1, 0.5f }, { 0, -1, 0 }, 0.999f) && trace_any(b, { 0.5f, 1, 0.5f }, { 0, -1, 0 }, 1.001f));
        const uint32_t not_world = kAllClasses & ~(1u << static_cast<uint32_t>(TraceClass::world));
        check("a ray sees only the classes it is given", !trace_any(b, { 0.5f, 1, 0.5f }, { 0, -1, 0 }, 10, not_world));
        const Bvh single = build_bvh({ square[2] }, one);
        check("a scene of one triangle is one node with one leaf and no second child", single.nodes.size() == 1 && single.leaves == 1
            && single.nodes[0].link[0] == kLeafBit && single.nodes[0].link[1] == kEmptyLink && trace_any(single, { 5.2f, 0.2f, -1 }, { 0, 0, 1 }, 5)
            && !trace_any(single, { 5.2f, 0.2f, -1 }, { 0, 0, -1 }, 5));
        const Bvh empty = build_bvh({}, one);
        check("an empty scene has no nodes, and rays through it hit nothing", empty.nodes.empty() && !trace_any(empty, { 0, 0, 0 }, { 0, 1, 0 }, 1));
        TraceTriangle wide = square[0];
        const Bvh flat = build_bvh(std::vector<TraceTriangle>(40, wide), one);
        check("40 triangles with one centroid split in the middle into leaves of at most 16", flat.leaves >= 3 && trace_any(flat, { 0.6f, 1, 0.3f }, { 0, -1, 0 }, 5));
    }
    std::printf("%s: %d failed\n", __FILE__, failures);
    return failures == 0 ? 0 : 1;
}
