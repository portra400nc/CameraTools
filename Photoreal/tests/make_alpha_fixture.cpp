// Cuts tests/fixtures/scene-085011-alpha-rays.bin, the rays alpha_test.cpp holds the ray's alpha caps to: the rays of a
// Deck frame's first dispatch, its first 4 tiles as build 204 traced them, that cross the most vegetation and foliage
// cards, and others spread over the rest, with the triangles near each. Every card gets an alpha record that discards
// everywhere, so a ray crosses every card on its way, the most alpha work the scene can ask of it.
//
//   c++ -std=c++17 -O2 -pthread -I .. make_alpha_fixture.cpp $(ls ../core/*.cpp | grep -v kernel.cpp) -o make_alpha_fixture
//   ./make_alpha_fixture <scene folder>/trace-scene.bin <trace folder> fixtures/scene-085011-alpha-rays.bin
//
// Run in tests/: it reads fixtures/68b05754fdce207c.ps.dxbc's program. The fixture: "PHAF", version 1, the ray count,
// then each ray (origin, direction, t_max, any_hit, classes: 9 words), the triangle count, then each triangle (its 3
// corners and its class: 10 words), little-endian, in the scene's frame.
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

    // The ray's t at the triangle, or -1.
    double hit_t(const TraceTriangle &t, const FixtureRay &r)
    {
        double e1[3], e2[3], s[3], p[3], q[3];
        for (int k = 0; k < 3; k++)
        {
            e1[k] = double(t.corner[1][k]) - t.corner[0][k];
            e2[k] = double(t.corner[2][k]) - t.corner[0][k];
            s[k] = double(r.o[k]) - t.corner[0][k];
        }
        const auto cross = [](const double *a, const double *b, double *c) {
            c[0] = a[1] * b[2] - a[2] * b[1];
            c[1] = a[2] * b[0] - a[0] * b[2];
            c[2] = a[0] * b[1] - a[1] * b[0];
        };
        const double d[3] = { r.d[0], r.d[1], r.d[2] };
        cross(d, e2, p);
        const double det = e1[0] * p[0] + e1[1] * p[1] + e1[2] * p[2];
        if (std::fabs(det) < 1e-12)
            return -1;
        const double u = (s[0] * p[0] + s[1] * p[1] + s[2] * p[2]) / det;
        cross(s, e1, q);
        const double v = (d[0] * q[0] + d[1] * q[1] + d[2] * q[2]) / det;
        const double tt = (e2[0] * q[0] + e2[1] * q[1] + e2[2] * q[2]) / det;
        return u >= 0 && v >= 0 && u + v <= 1 && tt > 1e-4 && tt < r.t_max ? tt : -1;
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
}

int main(int argc, char **argv)
{
    if (argc < 4)
    {
        std::fprintf(stderr, "usage: make_alpha_fixture <trace-scene.bin> <trace folder> <out>\n");
        return 2;
    }
    std::optional<TraceSceneFile> scene = parse_trace_scene(slurp(argv[1]));
    std::string why;
    const std::optional<TraceSave> save = read_trace_folder(argv[2], why);
    const std::string code = slurp("fixtures/68b05754fdce207c.ps.dxbc");
    const AlphaAnalysis cutoff = analyze_alpha(reinterpret_cast<const uint8_t *>(code.data()), code.size());
    if (!scene || !save || !cutoff.program)
        return std::fprintf(stderr, "cannot read the scene, the trace folder (%s) or the program\n", why.c_str()), 1;
    // Every card discards everywhere: a zero atlas against cb2[14].y = 0.5.
    AlphaBuffer buffer;
    const float half = 0.5f;
    uint32_t half_bits;
    std::memcpy(&half_bits, &half, 4);
    const uint32_t draw = buffer.add_draw(buffer.add_program(cutoff.program), 0, 1 | 1 << 3, { 0, half_bits, 0, 0 });
    for (TraceTriangle &t : scene->triangles)
    {
        const TraceClass c = info_class(t.info);
        if (c == TraceClass::vegetation || c == TraceClass::foliage)
        {
            const float uv[6] = { 0, 0, 1, 0, 0, 1 };
            t.info = triangle_info(c, buffer.add_record(draw, uv, 2));
        }
    }
    const std::vector<uint32_t> alpha = buffer.words(std::vector<uint8_t>(kAtlasTile * kAtlasTile, 0));
    const Bvh bvh = build_bvh(scene->triangles);

    TraceImage image;
    image.size = save->size;
    image.depth = save->depth;
    image.normals = save->normals;
    image.stencil = save->stencil;
    TraceConstants constants = save->constants;
    const TraceSettings settings;
    constants.trace_counts[0] = settings.ao_samples;
    constants.trace_counts[1] = settings.sun_samples;
    constants.trace_counts[2] = caster_classes(settings);
    constants.trace_render[3] = kGridStride;
    const std::vector<uint32_t> order = tile_order(image.size);
    std::vector<FixtureRay> rays;
    rays_out = &rays;
    for (uint32_t k = 0; k < 4; k++)
        for (uint32_t y = 0; y < kTileSize; y++)
            for (uint32_t x = 0; x < kTileSize; x++)
                trace_pixel(bvh, image, constants, (order[k] & 0xFFFF) * kTileSize + x, (order[k] >> 16) * kTileSize + y, {}, &alpha);
    rays_out = nullptr;

    // Each ray's cards before its first other triangle, and where that lies.
    struct Scored
    {
        uint32_t cards = 0;
        double end = 0;
        size_t ray = 0;
    };
    std::vector<Scored> scored(rays.size());
    for (size_t i = 0; i < rays.size(); i++)
    {
        double end = rays[i].t_max;
        for (const TraceTriangle &t : scene->triangles)
            if ((t.info & kOpaque) == kOpaque && (rays[i].mask >> (t.info >> 24 & 15) & 1))
            {
                const double h = hit_t(t, rays[i]);
                if (h > 0)
                    end = std::min(end, h);
            }
        uint32_t cards = 0;
        for (const TraceTriangle &t : scene->triangles)
            if ((t.info & kOpaque) != kOpaque && (rays[i].mask >> (t.info >> 24 & 15) & 1))
            {
                const double h = hit_t(t, rays[i]);
                cards += h > 0 && h < end;
            }
        scored[i] = { cards, end, i };
    }
    std::vector<Scored> by_cards = scored;
    std::stable_sort(by_cards.begin(), by_cards.end(), [](const Scored &a, const Scored &b) { return a.cards > b.cards; });
    std::vector<Scored> chosen(by_cards.begin(), by_cards.begin() + std::min<size_t>(32, by_cards.size()));
    for (size_t i = 0; i < 32 && !scored.empty(); i++)
        chosen.push_back(scored[(i * 2654435761u) % scored.size()]);

    std::vector<uint8_t> keep(scene->triangles.size(), 0);
    for (const Scored &s : chosen)
    {
        const double length = std::min<double>(rays[s.ray].t_max, s.end + 0.5);
        for (size_t t = 0; t < scene->triangles.size(); t++)
            keep[t] |= near_segment(scene->triangles[t], rays[s.ray], length, 0.05);
    }
    std::vector<uint32_t> words = { 0x46414850u, 1, static_cast<uint32_t>(chosen.size()) };
    const auto put = [&](float f) {
        uint32_t u;
        std::memcpy(&u, &f, 4);
        words.push_back(u);
    };
    for (const Scored &s : chosen)
    {
        const FixtureRay &r = rays[s.ray];
        for (float f : { r.o[0], r.o[1], r.o[2], r.d[0], r.d[1], r.d[2], r.t_max })
            put(f);
        words.push_back(r.any_hit);
        words.push_back(r.mask);
    }
    words.push_back(static_cast<uint32_t>(std::count(keep.begin(), keep.end(), 1)));
    for (size_t t = 0; t < scene->triangles.size(); t++)
        if (keep[t])
        {
            for (const Vec3f &c : scene->triangles[t].corner)
                for (float f : c)
                    put(f);
            words.push_back(static_cast<uint32_t>(info_class(scene->triangles[t].info)));
        }
    std::ofstream(argv[3], std::ios::binary).write(reinterpret_cast<const char *>(words.data()), static_cast<std::streamsize>(words.size() * 4));
    std::printf("%zu rays of %zu, the most crossing %u cards and the 32nd %u; %zu triangles\n", chosen.size(), rays.size(), by_cards[0].cards,
        by_cards[std::min<size_t>(31, by_cards.size() - 1)].cards, static_cast<size_t>(std::count(keep.begin(), keep.end(), 1)));
    return 0;
}
