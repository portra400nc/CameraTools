// check_bvh.sh's tool: the BVH on a whole scene capture's deduplicated triangles, a file of a uint32 count, nine float32
// a triangle and optionally a class byte a triangle after them (.work/photoreal/tracer/scene-194351-tris.bin, build
// 199's 2,377,274, in the OBJ frame, as dedupe_check.py wrote them); every triangle counts as world. Builds it
// on 1 and 8 threads and prints the times, checks both give the same hash, checks the traversal against every-triangle
// testing for 200 rays, measures the traversal's rays a second on one thread from the view camera as
// .work/photoreal/tracer/bvh_proto.cpp did, and checks that this optimized build gives the fixture's BVH the hash
// bvh_test.cpp expects. Prints PASS, FAIL and INFO lines like the tests.
#include "../core/bvh.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace enhancements;

namespace
{
    int failures = 0;

    void check(const char *name, bool ok)
    {
        std::printf("%s %s\n", ok ? "PASS" : "FAIL", name);
        failures += ok ? 0 : 1;
    }

    std::string slurp(const char *path)
    {
        std::ifstream file(path, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    }

    double seconds_since(std::chrono::steady_clock::time_point start)
    {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    }

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

    // A direction around normal, cosine-weighted.
    Vec3f cosine(Random &random, const Vec3f &normal)
    {
        const Vec3f side = unit(cross(std::fabs(normal[1]) < 0.9f ? Vec3f { 0, 1, 0 } : Vec3f { 1, 0, 0 }, normal)), up = cross(normal, side);
        const float r = std::sqrt(random.next()), phi = 6.2831853f * random.next();
        return unit(side * (r * std::cos(phi)) + up * (r * std::sin(phi)) + normal * std::sqrt(1 - r * r));
    }
}

int main(int argc, char **argv)
{
    if (argc < 3)
    {
        std::fprintf(stderr, "usage: check_bvh <triangles.bin> <fixture BVH hash, hex>\n");
        return 2;
    }
    const std::string bytes = slurp(argv[1]);
    uint32_t count = 0;
    if (bytes.size() >= 4)
        std::memcpy(&count, bytes.data(), 4);
    if (bytes.size() != 4 + size_t(count) * 36 && bytes.size() != 4 + size_t(count) * 37)
    {
        std::printf("FAIL %s is not a uint32 count and nine float32 a triangle, with or without a class byte each\n", argv[1]);
        return 1;
    }
    std::vector<TraceTriangle> scene(count);
    for (uint32_t i = 0; i < count; i++)
        std::memcpy(scene[i].corner.data(), bytes.data() + 4 + size_t(i) * 36, 36);

    BvhOptions one, eight;
    one.tasks = 1;
    eight.tasks = 8;
    auto start = std::chrono::steady_clock::now();
    const Bvh bvh = build_bvh(scene, one);
    const double serial = seconds_since(start);
    start = std::chrono::steady_clock::now();
    const Bvh parallel = build_bvh(scene, eight);
    const double threaded = seconds_since(start);
    BvhOptions three;
    three.tasks = 3;
    start = std::chrono::steady_clock::now();
    const Bvh deck = build_bvh(scene, three);
    std::printf("INFO built in %.2f s with the add-on's 3 tasks\n", seconds_since(start));
    std::printf("INFO %u triangles: built in %.2f s on 1 thread and %.2f s on 8; %zu nodes (%.1f MB), %u leaves, %.2f triangles a leaf, triangles %.1f MB, "
        "SAH cost %.1f, hash %016llx\n", count, serial, threaded, bvh.nodes.size(), bvh.nodes.size() * 64 / 1e6, bvh.leaves, double(count) / bvh.leaves,
        count * 48 / 1e6, bvh.sah, static_cast<unsigned long long>(bvh.hash));
    check("the whole scene's BVH is the same on 1, 3 and 8 threads", parallel.hash == bvh.hash && deck.hash == bvh.hash && parallel.nodes.size() == bvh.nodes.size());

    Random random;
    const Vec3f sun = unit({ 0.4299f, 1.0f, -1.3619f });    // the OBJ frame's z is Unity's mirrored
    uint32_t agreed = 0, rays = 0;
    for (int i = 0; i < 100; i++)
    {
        const TraceTriangle &t = scene[static_cast<size_t>(random.next() * count) % count];
        const Vec3f normal = unit(cross(t.corner[1] - t.corner[0], t.corner[2] - t.corner[0]));
        const Vec3f origin = (t.corner[0] + t.corner[1] + t.corner[2]) * (1.0f / 3) + normal * 1e-3f;
        for (const Vec3f &d : { cosine(random, normal), sun })
        {
            const std::optional<Hit> a = trace_nearest(bvh, origin, d, 1e5f), b = brute_nearest(bvh, origin, d, 1e5f);
            agreed += (!a && !b) || (a && b && (a->triangle == b->triangle || std::fabs(a->t - b->t) <= 1e-6f * b->t));
            rays++;
        }
    }
    check("200 rays: the traversal's nearest hit is every-triangle testing's", agreed == rays);

    // From the view camera at the origin: primary rays over a 70-degree view, then from each hit 8 cosine rays within
    // 2 m and 10 m, and one toward the sun, as bvh_proto.cpp measured; the hit's normal is the reverse ray's.
    const Vec3f forward = unit({ 0.0249f, -0.0497f, 0.9985f }), right = unit(cross({ 0, 1, 0 }, forward)), up = cross(forward, right);
    struct Kind { const char *name; uint64_t rays = 0, steps = 0; double seconds = 0; } kinds[4] = { { "primary" }, { "ao 2 m" }, { "ao 10 m" }, { "sun" } };
    const auto timed = [](Kind &kind, auto &&trace) {
        TraversalStats stats;
        const auto begin = std::chrono::steady_clock::now();
        const bool hit = trace(stats);
        kind.seconds += seconds_since(begin);
        kind.rays++;
        kind.steps += stats.steps;
        return hit;
    };
    for (int y = 0; y < 100; y++)
        for (int x = 0; x < 160; x++)
        {
            const Vec3f d = unit(forward + right * ((2 * (x + 0.5f) / 160 - 1) * 0.7f) + up * ((1 - 2 * (y + 0.5f) / 100) * 0.7f * 100 / 160));
            std::optional<Hit> primary;
            timed(kinds[0], [&](TraversalStats &s) { primary = trace_nearest(bvh, { 0, 0, 0 }, d, 1e5f, kAllClasses, &s); return primary.has_value(); });
            if (!primary)
                continue;
            const Vec3f normal = d * -1.0f, origin = d * primary->t + normal * 0.01f;
            for (int s = 0; s < 8; s++)
            {
                const Vec3f dir = cosine(random, normal);
                timed(kinds[1], [&](TraversalStats &st) { return trace_any(bvh, origin, dir, 2.0f, kAllClasses, &st); });
                timed(kinds[2], [&](TraversalStats &st) { return trace_any(bvh, origin, dir, 10.0f, kAllClasses, &st); });
            }
            timed(kinds[3], [&](TraversalStats &st) { return trace_any(bvh, origin, sun, 1e5f, kAllClasses, &st); });
        }
    uint64_t all = 0;
    double seconds = 0;
    for (const Kind &k : kinds)
    {
        std::printf("INFO   %-8s %8llu rays: %6.1f nodes a ray\n", k.name, static_cast<unsigned long long>(k.rays), double(k.steps) / k.rays);
        all += k.rays;
        seconds += k.seconds;
    }
    std::printf("INFO traced %llu rays in %.2f s on one thread: %.2f Mrays/s\n", static_cast<unsigned long long>(all), seconds, all / seconds / 1e6);

    // The fixture, as bvh_test.cpp builds it without optimization, must give the same bytes here.
    const std::string fixture = slurp("fixtures/scene-194351-near.bin");
    uint32_t fixture_count = 0;
    std::memcpy(&fixture_count, fixture.data() + 4, 4);
    static const uint32_t kStencils[] = { 0, 128, 129, 133, 136, 137, 144, 165, 200 };
    SceneBuilder builder;
    for (uint32_t i = 0; i < fixture_count; i++)
    {
        const char *at = fixture.data() + 8 + size_t(i) * 40;
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
    const Bvh small = build_bvh(builder.take(UINT64_MAX), one);
    check("an optimized build gives the fixture's BVH the hash an unoptimized one does", small.hash == std::stoull(argv[2], nullptr, 16));
    std::printf("%s: %d failed\n", __FILE__, failures);
    return failures == 0 ? 0 : 1;
}
