// Cuts tests/fixtures/scene-161003-meadow-rays.bin, the rays trace_scene_test.cpp holds the scene build's equal-depth
// rule to: rays of build 207's meadow frame that met their work cap, with the triangles their segments cross, each
// tagged with the draw kind and class the capture gave it.
//
//   c++ -std=c++17 -O2 -pthread -I .. make_meadow_fixture.cpp $(ls ../core/*.cpp | grep -v kernel.cpp) -o make_meadow_fixture
//   ./make_meadow_fixture <scene folder>/trace-scene.bin <trace folder> fixtures/scene-161003-meadow-rays.bin
//
// The scene is scene-20261012-161003 and the frame trace-20261012-161041. In that capture every equal-depth draw came
// back in its prepass draw's stream layout, so each of its triangles is one real corner and two misread ones, which lie
// within 0.2 m of the capture's camera, the scene's reference (README, "Misread equal-depth draws"); no other triangle of the
// scene has a corner within 1 m of it. The cut tells them by that: a triangle with a corner within 0.5 m of the
// reference is an equal-depth copy without a twin, one of the other class a prepass copy, and any other a main copy.
//
// It takes the kernel's own rays of the 64x64 tile whose rays met the cap most often, every 4th pixel each way, with
// whole cards, and keeps the 24 occlusion rays that did the most work, each with the triangles whose boxes lie within
// 0.05 m of it up to 0.5 m past where it stops in the scene without the misread copies. The fixture: "PMF1", the ray
// count, each ray (origin, direction, t_max, any_hit, classes without the alpha tests: 9 words), the triangle count,
// then each triangle as make_trace_fixture.py writes it (nine float32 corners, then uint8 face, kind and class and a
// zero byte), in the capture's order, little-endian, in the scene's frame.
#include <vector>

struct FixtureRay
{
    float o[3], d[3], t_max;
    uint32_t any_hit, mask;
};
static thread_local std::vector<FixtureRay> *rays_out = nullptr;
#define TRACE_RAY_HOOK(o, d, t, any, m) \
    (rays_out ? rays_out->push_back(FixtureRay { { o.x, o.y, o.z }, { d.x, d.y, d.z }, t, (any) ? 1u : 0u, m }) : (void)0)
#include "../core/kernel.cpp"

#include "../core/trace_files.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>

using namespace photoreal;

namespace
{
    std::string slurp(const std::string &path)
    {
        std::ifstream f(path, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    }

    // Whether the segment from o along d to length meets the triangle's box grown by margin.
    bool near_segment(const TraceTriangle &t, const FixtureRay &r, double length, double margin)
    {
        double enter = 0, leave = length;
        for (int k = 0; k < 3; k++)
        {
            const double lo = std::min({ t.corner[0][k], t.corner[1][k], t.corner[2][k] }) - margin;
            const double hi = std::max({ t.corner[0][k], t.corner[1][k], t.corner[2][k] }) + margin;
            if (std::fabs(r.d[k]) < 1e-12)
            {
                if (r.o[k] < lo || r.o[k] > hi)
                    return false;
                continue;
            }
            double a = (lo - r.o[k]) / r.d[k], b = (hi - r.o[k]) / r.d[k];
            if (a > b)
                std::swap(a, b);
            enter = std::max(enter, a);
            leave = std::min(leave, b);
        }
        return enter <= leave;
    }

    struct Traced
    {
        FixtureRay ray;
        TraversalStats stats;
        float end = 0;      // where it stops in the scene without the misread copies
    };
}

int main(int argc, char **argv)
{
    if (argc < 4)
    {
        std::fprintf(stderr, "usage: make_meadow_fixture <trace-scene.bin> <trace folder> <out>\n");
        return 2;
    }
    const std::optional<TraceSceneFile> scene = parse_trace_scene(slurp(argv[1]));
    std::string why;
    const std::optional<TraceSave> save = read_trace_folder(argv[2], why);
    if (!scene || !save)
        return std::fprintf(stderr, "cannot read the scene or the trace folder (%s)\n", why.c_str()), 1;
    const TraceConstants &c = save->constants;
    const auto misread = [](const TraceTriangle &t) {
        for (const Vec3f &corner : t.corner)
            if (std::hypot(corner[0], corner[1], corner[2]) < 0.5f)
                return true;
        return false;
    };
    std::vector<TraceTriangle> clean;
    for (const TraceTriangle &t : scene->triangles)
        if (!misread(t))
            clean.push_back(t);
    std::printf("%zu triangles, %zu of them misread equal-depth copies\n", scene->triangles.size(), scene->triangles.size() - clean.size());
    const Bvh bvh = build_bvh(scene->triangles), clean_bvh = build_bvh(clean);

    TraceImage image;
    image.size = save->size;
    image.depth = save->depth;
    image.normals = save->normals;
    image.stencil = save->stencil;
    TraceConstants constants = c;
    const TraceSettings settings;
    constants.trace_counts[0] = settings.ao_samples;
    constants.trace_counts[1] = settings.sun_samples;
    constants.trace_counts[2] = caster_classes(settings) & ~kAlphaTests;
    constants.trace_render[3] = 4;
    const uint32_t columns = (image.size.width + kTileSize - 1) / kTileSize, rows = (image.size.height + kTileSize - 1) / kTileSize;
    // The rays of a tile's every 4th pixel each way, each pixel with every ray kind, and how the whole scene treats them.
    const auto tile_rays = [&](uint32_t tile) {
        std::vector<FixtureRay> rays;
        rays_out = &rays;
        for (uint32_t y = 0; y < kTileSize; y += 4)
            for (uint32_t x = 0; x < kTileSize; x += 4)
            {
                const uint32_t px = tile % columns * kTileSize + x, py = tile / columns * kTileSize + y;
                if (px < image.size.width && py < image.size.height)
                    trace_pixel(bvh, image, constants, px, py, {}, nullptr);
            }
        rays_out = nullptr;
        std::vector<Traced> traced;
        for (const FixtureRay &r : rays)
        {
            Traced t { r, {}, r.t_max };
            const Vec3f o = { r.o[0], r.o[1], r.o[2] }, d = { r.d[0], r.d[1], r.d[2] };
            if (r.any_hit)
                trace_any(bvh, o, d, r.t_max, r.mask, &t.stats);
            else
                trace_nearest(bvh, o, d, r.t_max, r.mask, &t.stats);
            if (const std::optional<Hit> hit = trace_nearest(clean_bvh, o, d, r.t_max, r.mask))
                t.end = hit->t;
            traced.push_back(t);
        }
        return traced;
    };
    uint32_t best_tile = 0, best_capped = 0;
    for (uint32_t tile = 0; tile < columns * rows; tile++)
    {
        uint32_t capped = 0;
        for (const Traced &t : tile_rays(tile))
            capped += t.stats.guard;
        if (capped > best_capped)
        {
            best_capped = capped;
            best_tile = tile;
        }
    }
    std::vector<Traced> traced = tile_rays(best_tile);
    std::printf("tile %u (%u, %u): %zu rays of every 4th pixel, %u cut short by a cap\n", best_tile, best_tile % columns, best_tile / columns, traced.size(),
        best_capped);
    const float ao_distance = constants.trace_params[0];
    std::stable_sort(traced.begin(), traced.end(), [](const Traced &a, const Traced &b) { return a.stats.work > b.stats.work; });
    std::vector<Traced> chosen;
    for (const Traced &t : traced)
        if (!t.ray.any_hit && t.ray.t_max == ao_distance && chosen.size() < 24)
            chosen.push_back(t);

    std::vector<uint8_t> keep(scene->triangles.size(), 0);
    for (const Traced &t : chosen)
    {
        const double length = std::min<double>(t.ray.t_max, t.end + 0.5);
        for (size_t i = 0; i < scene->triangles.size(); i++)
            keep[i] |= near_segment(scene->triangles[i], t.ray, length, 0.05);
    }
    std::vector<uint32_t> words = { 0x31464D50u, static_cast<uint32_t>(chosen.size()) };
    const auto put = [&](float f) {
        uint32_t u;
        std::memcpy(&u, &f, 4);
        words.push_back(u);
    };
    uint32_t capped = 0;
    for (const Traced &t : chosen)
    {
        const FixtureRay &r = t.ray;
        for (float f : { r.o[0], r.o[1], r.o[2], r.d[0], r.d[1], r.d[2], r.t_max })
            put(f);
        words.push_back(r.any_hit);
        words.push_back(r.mask);
        capped += t.stats.guard;
    }
    const size_t count_at = words.size();
    words.push_back(0);
    uint32_t kept = 0, equal = 0;
    for (size_t i = 0; i < scene->triangles.size(); i++)
    {
        if (!keep[i])
            continue;
        const TraceTriangle &t = scene->triangles[i];
        for (const Vec3f &corner : t.corner)
            for (float f : corner)
                put(f);
        // DrawClass in core/scene.h: pre s128 s129 s133 s136 s137 s144 s165 other; kinds 0 pre, 1 equal, 2 main.
        static const uint32_t kDrawClass[] = { 1, 4, 5, 0, 3 };
        const TraceClass cls = info_class(t.info);
        const uint32_t draw_kind = misread(t) ? 1 : cls == TraceClass::other ? 0 : 2;
        words.push_back(6 | draw_kind << 8 | kDrawClass[static_cast<size_t>(cls)] << 16);
        kept++;
        equal += draw_kind == 1;
    }
    words[count_at] = kept;
    std::ofstream out(argv[3], std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char *>(words.data()), static_cast<std::streamsize>(words.size() * 4));
    std::printf("wrote %s: %zu rays, %u of them cut short by a cap in the whole scene, and %u triangles, %u of them misread equal-depth copies, %zu bytes\n",
        argv[3], chosen.size(), capped, kept, equal, words.size() * 4);
    return out ? 0 : 1;
}
