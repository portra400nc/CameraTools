// The gather kernel compiled as C++ (shaders/gather.hlsli through core/kernel.cpp): a white furnace, a wall above lit
// ground, the same ground under a roof and under a face's picture, a foliage card lit from behind, the energy bound over a
// courtyard, the gather's occlusion sample against the trace kernel's for the same pixel and index, its counters, the
// gather's constants, and its R11G11B10_FLOAT decode against core/light.h's.
#include "../core/trace.h"
#include "../enhancements.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

using namespace enhancements;

namespace
{
    int failures = 0;

    void check(const std::string &name, bool ok)
    {
        std::printf("%s %s\n", ok ? "PASS" : "FAIL", name.c_str());
        failures += ok ? 0 : 1;
    }

    bool near(float a, float b, float tolerance) { return std::fabs(a - b) <= tolerance; }

    void set_unity(float out[16], const double rows[16])
    {
        for (int r = 0; r < 4; r++)
            for (int c = 0; c < 4; c++)
                out[c * 4 + r] = float(rows[r * 4 + c]);
    }

    Vec3 cross(const Vec3 &a, const Vec3 &b) { return { a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0] }; }
    float dot(const Vec3 &a, const Vec3 &b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
    Vec3 unit(const Vec3 &a)
    {
        const float l = std::sqrt(dot(a, a));
        return { a[0] / l, a[1] / l, a[2] / l };
    }

    uint32_t encode_normal(const Vec3 &n)
    {
        uint32_t v = 0;
        for (int k = 0; k < 3; k++)
            v |= static_cast<uint32_t>(std::lround((n[k] * 0.5f + 0.5f) * 1023)) << (k * 10);
        return v;
    }

    TraceTriangle triangle(Vec3f a, Vec3f b, Vec3f c, uint32_t light, TraceClass cls = TraceClass::world)
    {
        TraceTriangle t;
        t.corner = { a, b, c };
        t.info = triangle_info(cls);
        t.light = light;
        return t;
    }

    void quad(std::vector<TraceTriangle> &out, Vec3f a, Vec3f b, Vec3f c, Vec3f d, uint32_t light, TraceClass cls = TraceClass::world)
    {
        out.push_back(triangle(a, b, c, light, cls));
        out.push_back(triangle(a, c, d, light, cls));
    }

    // A camera at eye looking along forward with up, 90 degrees vertical, a reversed infinite projection whose clip z is
    // 0.25, and its frame of size: each pixel's depth, normal (the surface's, facing the camera) and stencil (137 on
    // foliage, else 128) cast through the scene, the reference at the world's origin.
    struct Frame
    {
        Camera camera;
        TraceImage image;
    };

    Frame cast(const Bvh &bvh, const Vec3 &eye, const Vec3 &forward, const Vec3 &up, Size size)
    {
        Frame f;
        const Vec3 right = cross(up, forward);
        const double view[16] = { right[0], right[1], right[2], -dot(right, eye), up[0], up[1], up[2], -dot(up, eye), -forward[0], -forward[1], -forward[2],
            dot(forward, eye), 0, 0, 0, 1 };
        const double aspect = double(size.width) / size.height;
        const double projection[16] = { 1 / aspect, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0.25, 0, 0, -1, 0 };
        set_unity(f.camera.world_to_view, view);
        set_unity(f.camera.view_to_clip, projection);
        f.image.size = size;
        f.image.depth.assign(size_t(size.width) * size.height, 0);
        f.image.normals.assign(f.image.depth.size(), 0);
        f.image.stencil.assign(f.image.depth.size(), 0);
        for (uint32_t y = 0; y < size.height; y++)
            for (uint32_t x = 0; x < size.width; x++)
            {
                const float vx = float(((x + 0.5) / size.width * 2 - 1) * aspect), vy = float(1 - (y + 0.5) / size.height * 2);
                const Vec3 d = { right[0] * vx + up[0] * vy + forward[0], right[1] * vx + up[1] * vy + forward[1], right[2] * vx + up[2] * vy + forward[2] };
                const std::optional<Hit> hit = trace_nearest(bvh, eye, d, 1e5f);
                if (!hit)
                    continue;
                const BvhTriangle &t = bvh.triangles[hit->triangle];
                Vec3 n = unit(cross({ t.e1[0], t.e1[1], t.e1[2] }, { t.e2[0], t.e2[1], t.e2[2] }));
                if (dot(n, d) > 0)
                    n = { -n[0], -n[1], -n[2] };
                const size_t at = size_t(y) * size.width + x;
                f.image.depth[at] = 0.25f / hit->t;
                f.image.normals[at] = encode_normal(n);
                f.image.stencil[at] = info_class(t.info) == TraceClass::foliage ? 137 : 128;
            }
        return f;
    }

    // The sums of samples gather dispatches, from none.
    struct Sums
    {
        std::array<float, 4> history {}, hit {}, sky {};
        uint64_t lit = 0;
    };

    Sums gather(const Bvh &bvh, const TraceImage &image, const TraceConstants &c, const GatherConstants &g, const std::vector<float> &sky, uint32_t x, uint32_t y,
        uint32_t samples, std::vector<std::array<float, 4>> *lights = nullptr, const GatherLights *traced = nullptr)
    {
        Sums s;
        for (uint32_t i = 0; i < samples; i++)
        {
            const GatherPixel p = gather_pixel(bvh, image, c, g, sky, x, y, s.history, s.hit, s.sky, nullptr, nullptr, traced);
            s.history = p.history;
            s.hit = p.hit;
            s.sky = p.sky;
            s.lit += p.lit;
            if (lights)
                lights->push_back(p.light);
        }
        return s;
    }

    std::vector<float> even_sky(float value)
    {
        std::vector<float> sky(size_t(kSkyMapSide) * kSkyMapSide * 4, value);
        for (size_t i = 3; i < sky.size(); i += 4)
            sky[i] = 1;
        return sky;
    }

    GatherConstants constants(const Vec3 &sun, const Vec3 &shade, float ambient, uint32_t samples)
    {
        GatherSettings settings;
        settings.samples = samples;
        settings.hit_ambient = 1;
        SunLight light;
        light.color = sun;
        light.shade = shade;
        AmbientFit fit;
        fit.rows[0] = { ambient, ambient, ambient };
        return gather_constants(settings, light, fit);
    }
}

int main()
{
    const uint32_t white = pack_light({ 1, 1, 1 }, AlbedoSource::direct);
    {
        // A box 6 m wide and 3 m high around the camera, open above, every surface white. Its light is 1 from every side
        // and its sky 1, without a sun, so every sample brings exactly 1.
        std::vector<TraceTriangle> box;
        quad(box, { -3, 0, -3 }, { 3, 0, -3 }, { 3, 0, 3 }, { -3, 0, 3 }, white);
        quad(box, { -3, 0, -3 }, { -3, 0, 3 }, { -3, 3, 3 }, { -3, 3, -3 }, white);
        quad(box, { 3, 0, -3 }, { 3, 3, -3 }, { 3, 3, 3 }, { 3, 0, 3 }, white);
        quad(box, { -3, 0, -3 }, { -3, 3, -3 }, { 3, 3, -3 }, { 3, 0, -3 }, white);
        quad(box, { -3, 0, 3 }, { 3, 0, 3 }, { 3, 3, 3 }, { -3, 3, 3 }, white);
        const Bvh bvh = build_bvh(box);
        const Frame f = cast(bvh, { 0.3f, 2, 0.2f }, unit({ 0.2f, -1, 0.1f }), unit({ 0, 0.1f, 1 }), { 16, 16 });
        TraceSettings trace;
        const TraceConstants c = *trace_constants(trace, 1, f.camera, false, f.image.size, { 0, 0, 0 }, { 0, 0, 0 }, 3);
        const GatherConstants g = constants({ 0, 0, 0 }, { 0, 0, 0 }, 1, 64);
        const std::vector<float> sky = even_sky(1);
        bool exact = true, mixed = false;
        uint32_t pixels = 0;
        for (uint32_t y = 0; y < 16; y += 3)
            for (uint32_t x = 0; x < 16; x += 3)
            {
                std::vector<std::array<float, 4>> lights;
                const Sums s = gather(bvh, f.image, c, g, sky, x, y, 64, &lights);
                if (s.history[2] != 64)
                    continue;
                pixels++;
                for (const auto &l : lights)
                    exact &= l[0] == 1 && l[1] == 1 && l[2] == 1;
                exact &= (s.hit[0] + s.sky[0]) / 64 == 1 && (s.hit[2] + s.sky[2]) / 64 == 1 && s.hit[3] == 0 && s.sky[3] + s.hit[0] == 64;
                mixed |= s.sky[3] > 0 && s.sky[3] < 64;
            }
        check("a white furnace: every sample of " + std::to_string(pixels) + " pixels brings exactly 1, and the hits' and the sky's sums over the samples are 1",
            exact && mixed && pixels > 20);
    }
    // A wall facing +x at x = 0, seen from 2 m away, its pixel (3, 3) 1.25 m above a ground 2 km wide of albedo 0.5,
    // under a sun 30 degrees up and no ambient light: half its rays meet the ground, a fourth of whose light comes back.
    const uint32_t half = pack_light({ 0.5f, 0.5f, 0.5f }, AlbedoSource::direct);
    const float a = light_albedo(half)[0];
    const Vec3 sun = { std::cos(0.5235988f), std::sin(0.5235988f), 0 };
    std::vector<TraceTriangle> ground;
    quad(ground, { -1000, 0, -1000 }, { -1000, 0, 1000 }, { 1000, 0, 1000 }, { 1000, 0, -1000 }, half);
    TraceImage wall;
    wall.size = { 8, 8 };
    wall.depth.assign(64, 0.125f);
    wall.normals.assign(64, encode_normal({ 1, 0, 0 }));
    wall.stencil.assign(64, 128);
    Camera side {};
    {
        const double view[16] = { 0, 0, 1, 0, 0, 1, 0, -1, 1, 0, 0, -2, 0, 0, 0, 1 };
        const double projection[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0.25, 0, 0, -1, 0 };
        set_unity(side.world_to_view, view);
        set_unity(side.view_to_clip, projection);
    }
    TraceSettings trace;
    trace.sun_size = 0;
    const TraceConstants c = *trace_constants(trace, 1, side, false, wall.size, { 0, 0, 0 }, sun, 5);
    const GatherConstants g = constants({ 1, 1, 1 }, sun, 0, 4096);
    const std::vector<float> sky = even_sky(0.7f);
    {
        // The game's lights baked on the ground: 3 m above it a light of range 1000 lights its hits by the law at their
        // centroid. From below, the ground's back, nothing comes. A blocker shadows the light only if the game shadows it.
        std::vector<TraceTriangle> floor, blocked;
        quad(floor, { -1000, 0, -1000 }, { -1000, 0, 1000 }, { 1000, 0, 1000 }, { 1000, 0, -1000 }, half);
        blocked = floor;
        const size_t blocker = blocked.size();
        quad(blocked, { -1000, 1, -1000 }, { -1000, 1, 1000 }, { 1000, 1, 1000 }, { 1000, 1, -1000 }, half);
        EnhancementsShotLight raw {};
        raw.position[1] = 3;
        raw.range = 1000;
        raw.color[0] = raw.color[1] = raw.color[2] = 2;
        raw.cos_outer = raw.cos_inner = -2;
        const auto bake = [&](const std::vector<TraceTriangle> &triangles, uint32_t flags) {
            raw.reserved[0] = flags;
            Bvh b = build_bvh(triangles);
            const uint64_t hash = b.hash;
            const GameLightCounts counts = bake_game_lights(b, parse_trace_lights(&raw, 1, ENHANCEMENTS_MAX_GAME_LIGHTS), { 0, 0, 0 }, nullptr, 1);
            check("the bake leaves the scene's hash", b.hash == hash);
            return std::make_pair(std::move(b), counts);
        };
        const auto lit_word = [&](const Bvh &b, float y) {
            for (const BvhTriangle &t : b.triangles)
                if (t.v0[1] == y)
                    return t.pad1;
            return 0xFFFFFFFFu;
        };
        const auto [plain, plain_counts] = bake(floor, 0);
        const auto [shadowless, shadowless_counts] = bake(blocked, 0);
        const auto [shadowed, shadowed_counts] = bake(blocked, ENHANCEMENTS_SHOT_LIGHT_SHADOWS);
        (void)blocker;
        const BvhTriangle &first = plain.triangles[0];
        const Vec3d centroid = { first.v0[0] + (first.e1[0] + first.e2[0]) / 3.0, 0, first.v0[2] + (first.e1[2] + first.e2[2]) / 3.0 };
        const double distance = std::sqrt(centroid[0] * centroid[0] + 9 + centroid[2] * centroid[2]);
        const double x = distance / 1000;
        const float want_light = float(std::pow(2.0, 2.2) * (3 / distance) * 1 / (1 + 25 * x * x)) * a;
        const Vec3 unpacked = kernel_albedo(first.pad1);
        check("the bake holds the law's light at a triangle's centroid times its albedo, with no ray for a light the game does not shadow",
            near(unpacked[0] * kBakedLightRange, want_light, 0.05f * want_light) && plain_counts.lit == 2 && plain_counts.rays == 0);
        check("a light the game draws without shadows passes a blocker, as the game shows it; one with shadows stops at it",
            lit_word(shadowless, 0) != 0 && lit_word(shadowless, 0) != 0xFFFFFFFFu && shadowed_counts.rays > 0 && lit_word(shadowed, 0) == 0 && lit_word(shadowed, 1) != 0);
        check("the words are 10 bits a channel of the square root over the range", pack_baked_light({ 16, 4, 0 }) == (1023u | 512u << 10));
        (void)shadowless_counts;
        // The same ground lights a wall's bounce through the kernel: the sun gone, so all of it is the bake's.
        const GatherConstants dark = constants({ 0, 0, 0 }, sun, 0, 4096);
        const float flat = 0.2f;
        Bvh front = build_bvh(ground);
        for (BvhTriangle &t : front.triangles)
            t.pad1 = pack_baked_light({ flat, flat, flat });
        const float seen = gather(front, wall, c, dark, sky, 3, 3, 4096).hit[0] / 4096;
        std::vector<TraceTriangle> flipped;
        quad(flipped, { -1000, 0, -1000 }, { 1000, 0, -1000 }, { 1000, 0, 1000 }, { -1000, 0, 1000 }, half);
        Bvh back = build_bvh(flipped);
        for (BvhTriangle &t : back.triangles)
            t.pad1 = pack_baked_light({ flat, flat, flat });
        const float behind = gather(back, wall, c, dark, sky, 3, 3, 4096).hit[0] / 4096;
        check("a hit on a triangle's front takes its baked light, half the wall's rays times 0.2, and one on its back none",
            near(seen, 0.5f * flat, 0.01f) && behind == 0.0f);
    }
    {
        const Bvh bvh = build_bvh(ground);
        const Sums s = gather(bvh, wall, c, g, sky, 3, 3, 4096);
        const float vis = s.sky[3] / 4096, bounce = s.hit[0] / 4096, want = 0.5f * a * 0.5f;
        std::printf("INFO above lit ground: sky visibility %.5f, bounce %.5f (want %.5f)\n", vis, bounce, want);
        check("above lit ground at 4096 samples: the sky's share 0.5 and the bounce 0.5 x 0.5 x S x sin 30, within 3 standard errors",
            near(vis, 0.5f, 3 * 0.5f / 64) && near(bounce, want, 3 * want / 64) && s.hit[3] == float(s.lit) && s.lit == 4096 - uint64_t(s.sky[3])
                && near(s.sky[0] / s.sky[3], 0.7f, 1e-5f));
        std::vector<TraceTriangle> roofed = ground;
        quad(roofed, { -1000, 10, -1000 }, { 1000, 10, -1000 }, { 1000, 10, 1000 }, { -1000, 10, 1000 }, half);
        const Bvh under = build_bvh(roofed);
        const Sums r = gather(under, wall, c, g, sky, 3, 3, 512);
        check("the same ground under a roof 10 m up brings nothing: the roof's underside faces away from the sun, the ground is in its shadow, and "
              "only rays within half a degree of the horizon pass between them",
            r.hit[0] == 0 && r.hit[1] == 0 && r.hit[3] == 0 && r.lit == 0 && r.sky[3] < 512 / 100);
        std::vector<TraceTriangle> lifted = ground;
        for (TraceTriangle &t : lifted)
            t.light = pack_light({ 0.5f, 0.5f, 0.5f }, AlbedoSource::material);
        const Bvh material = build_bvh(lifted);
        uint32_t direct = 0, by_material = 0;
        std::array<float, 4> history {}, hit {}, sky_sums {};
        for (uint32_t i = 0; i < 64; i++)
        {
            const GatherPixel p = gather_pixel(material, wall, c, g, sky, 3, 3, history, hit, sky_sums);
            history = p.history;
            hit = p.hit;
            sky_sums = p.sky;
            direct += p.counters[static_cast<size_t>(GatherCounter::direct)];
            by_material += p.counters[static_cast<size_t>(GatherCounter::material)];
        }
        check("the gather counts a hit by its triangle's albedo source, and 2 rays a lit hit", direct == 0 && by_material == 64 - uint32_t(sky_sums[3])
            && by_material > 20);

        // A face 3 m above the ground at x = 3 looking straight down, 90 degrees across x and 1.6 times that across z, its
        // picture (0.75, 0.375, 0.1875) everywhere: a hit it shows takes that, with no sun ray; a hit beyond it the
        // albedo's light and its sun ray.
        Camera down {};
        {
            const double view[16] = { 0, 0, 1, 0, 1, 0, 0, -3, 0, 1, 0, -3, 0, 0, 0, 1 };
            const double projection[16] = { 1 / 1.6, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0.25, 0, 0, -1, 0 };
            set_unity(down.world_to_view, view);
            set_unity(down.view_to_clip, projection);
        }
        FaceImages picture = *face_images(kFaceView, *scene_camera(down, false), { 32, 20 });
        picture.depth.assign(640, 0.25f / 3);
        picture.hdr.assign(640, hdr_stored({ 0.75f, 0.375f, 0.1875f }));
        const std::vector<FaceLight> faces = face_lights({ picture }, { 0, 0, 0 });
        uint32_t face_lit = 0, sun_lit = 0;
        bool face_right = true, sun_right = true;
        std::array<float, 4> fh {}, fhit {}, fsky {};
        for (uint32_t i = 0; i < 256; i++)
        {
            const GatherPixel p = gather_pixel(bvh, wall, c, g, sky, 3, 3, fh, fhit, fsky, nullptr, &faces);
            fh = p.history;
            fhit = p.hit;
            fsky = p.sky;
            if (p.light[3] != 1)
                continue;
            if (p.counters[static_cast<size_t>(GatherCounter::face)] == 1)
            {
                face_lit++;
                face_right &= p.light[0] == 0.75f && p.light[1] == 0.375f && p.light[2] == 0.1875f && p.counters[0] == 1 && !p.lit
                    && p.counters[static_cast<size_t>(GatherCounter::direct)] == 0;
            }
            else
            {
                sun_lit++;
                sun_right &= p.light[0] == a * 0.5f && p.counters[0] == 2 && p.lit && p.counters[static_cast<size_t>(GatherCounter::direct)] == 1;
            }
        }
        std::printf("INFO under the face's picture: %u hits it shows, %u beyond it\n", face_lit, sun_lit);
        check("a hit the face shows takes its picture's (0.75, 0.375, 0.1875) and shoots 1 ray, counted as the face's", face_right && face_lit > 20);
        check("a hit the face does not show takes the albedo times the sun at 30 degrees, 0.5 a, and shoots 2 rays", sun_right && sun_lit > 20);

        // Traced lights over the lit ground: a point 0.5 m up at x = 2, range 8.
        const auto point_at = [](float radiance, float x = 2) {
            EnhancementsShotLight l {};
            l.position[0] = x;
            l.position[1] = 0.5f;
            l.range = 8;
            l.color[0] = l.color[1] = l.color[2] = radiance;
            l.cos_outer = l.cos_inner = -2;
            return l;
        };
        const EnhancementsShotLight one[] = { point_at(2) }, other[] = { point_at(1, 4) }, two[] = { point_at(2), point_at(1, 4) };
        const GatherLights lit_one = gather_lights(parse_trace_lights(one, 1), { 0, 0, 0 });
        const GatherLights lit_other = gather_lights(parse_trace_lights(other, 1), { 0, 0, 0 });
        const GatherLights lit_two = gather_lights(parse_trace_lights(two, 2), { 0, 0, 0 });
        const GatherConstants dark = constants({ 0, 0, 0 }, sun, 0, 4096);
        // The standard error of a run's mean hit light, from its samples.
        const auto error_of = [](const std::vector<std::array<float, 4>> &samples) {
            double sum = 0, squares = 0;
            for (const auto &v : samples)
            {
                const double x = v[3] == 1 ? v[0] : 0;
                sum += x;
                squares += x * x;
            }
            const double n = double(samples.size()), mean = sum / n;
            return float(std::sqrt(std::max(squares / n - mean * mean, 0.0) / n));
        };
        std::vector<std::array<float, 4>> s_one, s_other, s_two;
        const Sums none = gather(bvh, wall, c, dark, sky, 3, 3, 4096);
        const Sums by_one = gather(bvh, wall, c, dark, sky, 3, 3, 4096, &s_one, &lit_one);
        const Sums by_other = gather(bvh, wall, c, dark, sky, 3, 3, 4096, &s_other, &lit_other);
        const Sums by_two = gather(bvh, wall, c, dark, sky, 3, 3, 4096, &s_two, &lit_two);
        const float m1 = by_one.hit[0] / 4096, m2 = by_two.hit[0] / 4096, apart_two = m1 + by_other.hit[0] / 4096;
        std::printf("INFO the ground lit by traced lights: bounce %.5f from one, %.5f from the other, %.5f from both, %.5f with none\n", m1,
            by_other.hit[0] / 4096, m2, none.hit[0] / 4096);
        check("with the sun off, no traced light brings no bounce, and one lights the ground the gather's rays meet within its range",
            none.hit[0] == 0 && none.lit == 0 && m1 > 0.01f && by_one.lit > 0 && by_one.lit <= 4096 - uint64_t(by_one.sky[3]));
        const float spread = std::sqrt(error_of(s_one) * error_of(s_one) + error_of(s_other) * error_of(s_other) + error_of(s_two) * error_of(s_two));
        std::printf("INFO both lights against each alone: %.5f against %.5f, standard error %.5f\n", m2, apart_two, spread);
        check("two lights at two places bring what each brings alone, the chosen one's light divided by the chance of its choice, within 3 standard errors",
            near(m2, apart_two, 3 * spread));
        const Sums sun_and_light = gather(bvh, wall, c, g, sky, 3, 3, 4096, nullptr, &lit_one);
        const Sums sun_alone = gather(bvh, wall, c, g, sky, 3, 3, 4096);
        const float sum = sun_and_light.hit[0] / 4096, apart = sun_alone.hit[0] / 4096 + m1;
        std::printf("INFO the sun and the light together: %.5f, apart %.5f\n", sum, apart);
        check("the sun and a light together bring what each brings alone, one shadow ray chosen between them, within 4%", near(sum, apart, 0.04f * apart));
    }
    {
        // A foliage card 1 m in front of the wall, facing it, with the sun behind it: the card takes the sun at half on
        // the side the rays meet. A world card of the same shape is dark on that side.
        const uint32_t green = pack_light({ 0.2f, 0.8f, 0.2f }, AlbedoSource::direct);
        for (TraceClass cls : { TraceClass::foliage, TraceClass::world })
        {
            std::vector<TraceTriangle> card;
            quad(card, { 1, 0.25f, -1 }, { 1, 0.25f, 1 }, { 1, 2.25f, 1 }, { 1, 2.25f, -1 }, green, cls);
            const Bvh bvh = build_bvh(card);
            std::vector<std::array<float, 4>> lights;
            gather(bvh, wall, c, g, sky, 3, 3, 256, &lights);
            uint32_t hits = 0;
            bool right = true;
            const Vec3f albedo = light_albedo(green);
            for (const auto &l : lights)
                if (l[3] == 1)
                {
                    hits++;
                    right &= cls == TraceClass::foliage ? l[0] == albedo[0] * 0.5f && l[1] == albedo[1] * 0.5f : l[0] == 0 && l[1] == 0;
                }
            check(cls == TraceClass::foliage ? "a foliage card lit from behind gives each ray that meets it its albedo times the sun at half, " + std::to_string(hits)
                        + " hits"
                                             : "a world card of that shape gives its back side no sun",
                right && hits > 50);
        }
    }
    {
        // The courtyard: a floor, three walls 4 m high and a cube, of albedos up to 1, the sun low over the open side, an
        // ambient light at most the sky's 0.4 and a sky of 0.4: each sample brings at most 0.4 from the sky, or from a
        // surface 0.4 and the sun, so sky visibility x 0.4 + bounce is at most 0.4 + (1 - visibility) x S.
        std::vector<TraceTriangle> yard;
        quad(yard, { -5, 0, -5 }, { 5, 0, -5 }, { 5, 0, 5 }, { -5, 0, 5 }, pack_light({ 0.6f, 0.5f, 0.4f }, AlbedoSource::direct));
        quad(yard, { -5, 0, -5 }, { -5, 0, 5 }, { -5, 4, 5 }, { -5, 4, -5 }, white);
        quad(yard, { 5, 0, -5 }, { 5, 4, -5 }, { 5, 4, 5 }, { 5, 0, 5 }, pack_light({ 0.9f, 0.2f, 0.2f }, AlbedoSource::direct));
        quad(yard, { -5, 0, -5 }, { -5, 4, -5 }, { 5, 4, -5 }, { 5, 0, -5 }, pack_light({ 0.3f, 0.3f, 0.9f }, AlbedoSource::direct));
        quad(yard, { 0.5f, 0, -1.5f }, { 1.5f, 0, -1.5f }, { 1.5f, 1, -1.5f }, { 0.5f, 1, -1.5f }, white);
        quad(yard, { 0.5f, 1, -1.5f }, { 1.5f, 1, -1.5f }, { 1.5f, 1, -0.5f }, { 0.5f, 1, -0.5f }, white);
        const Bvh bvh = build_bvh(yard);
        const Frame f = cast(bvh, { 0, 1.6f, 4 }, unit({ 0, -0.3f, -1 }), unit({ 0, 1, -0.3f }), { 24, 16 });
        TraceSettings courtyard;
        const Vec3 low = unit({ 0.5f, 0.4f, 0.6f });
        const TraceConstants cy = *trace_constants(courtyard, 1, f.camera, false, f.image.size, { 0, 0, 0 }, low, 11);
        SunLight light;
        light.color = { 2, 2, 2 };
        light.shade = low;
        AmbientFit fit;
        fit.rows = { { { 0.3f, 0.3f, 0.3f }, { 0, 0, 0 }, { 0.1f, 0.1f, 0.1f }, { 0, 0, 0 } } };
        GatherSettings settings;
        settings.hit_ambient = 1;
        settings.samples = 48;
        const GatherConstants gy = gather_constants(settings, light, fit);
        const std::vector<float> sky04 = even_sky(0.4f);
        bool bounded = true, occlusion_same = true;
        uint32_t pixels = 0, compared = 0;
        double worst = -1;
        for (uint32_t y = 0; y < 16; y++)
            for (uint32_t x = 0; x < 24; x++)
            {
                const Sums s = gather(bvh, f.image, cy, gy, sky04, x, y, 48);
                if (s.history[2] == 0)
                    continue;
                pixels++;
                const float vis = s.sky[3] / 48;
                for (int k = 0; k < 3; k++)
                {
                    const double excess = vis * 0.4 + s.hit[k] / 48 - (0.4 + (1 - vis) * 2);
                    worst = std::max(worst, excess);
                    bounded &= excess <= 1e-5;
                }
                // The trace kernel's occlusion ray of each index against the gather's.
                for (uint32_t index = 0; index < 8; index++)
                {
                    const PixelTrace t = trace_pixel(bvh, f.image, cy, x, y, { 0, 0, float(index), 1e9f });
                    TraceConstants one = cy;
                    const GatherPixel p = gather_pixel(bvh, f.image, one, gy, sky04, x, y, { 0, 0, float(index), 0 }, {}, {});
                    occlusion_same &= t.history[0] == p.occlusion;
                    compared++;
                }
            }
        std::printf("INFO the courtyard: %u pixels, the bound's largest excess %.6f\n", pixels, worst);
        check("over the courtyard, sky visibility x L + bounce never exceeds L + (1 - visibility) x S", bounded && pixels > 300);
        check("the gather's occlusion sample is the trace kernel's, to the bit, for " + std::to_string(compared) + " pixels and indexes", occlusion_same && compared > 2400);
        const GatherPixel done = gather_pixel(bvh, f.image, cy, gy, sky04, 12, 8, { 3, 0, 48, 0 }, {}, {});
        check("a pixel with all its gather samples takes none", !done.world && done.history[2] == 48 && done.counters[0] == 0);
    }
    {
        GatherSettings settings;
        settings.hit_ambient = 0.5f;
        SunLight light;
        light.color = { 2.8f, 2.5f, 1.1f };
        light.shade = { 0, 2, 0 };
        AmbientFit fit;
        fit.rows = { { { 0.2f, 0.4f, 0.6f }, { 0, 0, 0 }, { 0.1f, 0.2f, 0.3f }, { 0, 0, 0 } } };
        const GatherConstants k = gather_constants(settings, light, fit);
        check("the gather's constants: the sun's color, the shading direction made unit, the ambient rows times hit_ambient, 64 samples",
            k.sun[0] == 2.8f && k.sun[2] == 1.1f && k.shade[1] == 1 && k.ambient[0][2] == 0.3f && k.ambient[2][1] == 0.1f && k.counts[0] == 64);
        light.shade = { 0, 0, 0 };
        const GatherConstants dark = gather_constants(settings, light, fit);
        check("without a sun, no sun color either", dark.sun[0] == 0 && dark.shade[1] == 0);
        Camera camera {};
        const double identity[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
        set_unity(camera.world_to_view, identity);
        const TraceKey off = trace_key(1, { 8, 8 }, camera, { 0, 1, 0 }, TraceSettings {}), on = trace_key(1, { 8, 8 }, camera, { 0, 1, 0 }, TraceSettings {}, &k);
        GatherConstants brighter = k;
        brighter.sun[0] += 0.01f;
        GatherConstants more = k;
        more.counts[0] = 512;
        check("the samples start again when the gather goes on or off, or its sun or ambient light changes, not for more samples",
            trace_reset(off, on) && trace_reset(on, trace_key(1, { 8, 8 }, camera, { 0, 1, 0 }, TraceSettings {}, &brighter))
                && !trace_reset(on, trace_key(1, { 8, 8 }, camera, { 0, 1, 0 }, TraceSettings {}, &more)) && !trace_reset(on, on));
        // A sun that brightens by 0.6e-5 a frame: no frame differs enough from the last, the second does from the start.
        GatherConstants drift1 = k, drift2 = k;
        drift1.sun[0] += 6e-6f;
        drift1.counts[0] = 256;
        drift2.sun[0] += 1.2e-5f;
        const TraceKey key1 = trace_key(1, { 8, 8 }, camera, { 0, 1, 0 }, TraceSettings {}, &drift1), key2 = trace_key(1, { 8, 8 }, camera, { 0, 1, 0 }, TraceSettings {}, &drift2);
        const GatherConstants held = held_gather(k, drift1);
        check("a light that drifts starts the samples again once it is 1e-5 from what they started with, though no frame is from the last, and until "
              "then they keep the start's sun and ambient rows with the frame's samples",
            !trace_reset(on, key1) && !trace_reset(key1, key2) && trace_reset(on, key2) && held.sun[0] == k.sun[0] && held.shade[1] == k.shade[1]
                && held.ambient[2][1] == k.ambient[2][1] && held.counts[0] == 256);
        TraceSettings t;
        GatherSettings gs;
        check("the target: the larger of the gather's and the sun's samples while gathering, else of the occlusion's and the sun's",
            (gs.samples = 128, trace_target(t, gs, true)) == 128 && trace_target(t, gs, false) == 64 && (gs.samples = 16, trace_target(t, gs, true)) == 32);
        check("the trace kernel runs beside the gather while the sun has samples to take, then every 8th pass",
            trace_kernel_runs(0, 32) && trace_kernel_runs(31, 32) && trace_kernel_runs(32, 32) && !trace_kernel_runs(33, 32) && !trace_kernel_runs(39, 32)
                && trace_kernel_runs(40, 32));
        check("the error: 1.25 x deviations / (means x sqrt(samples))", near(float(gather_error(1000, 10000, 64)), 0.015625f, 1e-7f) && gather_error(5, 0, 64) == 0);
    }
    {
        // Every channel's infinity reads as its largest value and NaN as 0, as core/light.h's hdr_rgb has them.
        std::mt19937 random(20261031);
        bool same = true;
        for (int i = 0; i < 10000; i++)
        {
            const uint32_t word = random();
            const Vec3 kernel = kernel_hdr_rgb(word), cpu = hdr_rgb(word);
            same &= std::memcmp(kernel.data(), cpu.data(), sizeof kernel) == 0;
        }
        check("the kernel decodes 10,000 random R11G11B10_FLOAT words to hdr_rgb's bits", same);
        check("the kernel reads each channel's infinity as its largest value, 65024 or 64512, and NaN as 0",
            kernel_hdr_rgb(0x7C0u | 0x7C0u << 11 | 0x3E0u << 22) == Vec3 { 65024, 65024, 64512 } && kernel_hdr_rgb(0x7C1u | 0x7FFu << 11 | 0x3E1u << 22) == Vec3 { 0, 0, 0 });
    }
    {
        // The measured law: a light of color times intensity 2, range 8, 2 m above a floor point; 7.5 m above it in its fade; a
        // spot aimed down with a 60 degree cone and a 30 degree inner cone.
        const auto light_at = [](float y, float cos_outer, float cos_inner) {
            EnhancementsShotLight l {};
            l.position[1] = y;
            l.range = 8;
            l.color[0] = l.color[1] = l.color[2] = 2;
            l.cos_outer = cos_outer;
            l.cos_inner = cos_inner;
            l.direction[1] = -1;
            return l;
        };
        const EnhancementsShotLight raw[] = { light_at(2, -2, -2), light_at(7.5f, -2, -2), light_at(2, 0.8660254f, 0.9659258f) };
        const GatherLights lights = gather_lights(parse_trace_lights(raw, 3), { 0, 0, 0 });
        const Vec3 up = { 0, 1, 0 };
        const float power = std::pow(2.0f, 2.2f);
        const float full = kernel_local_light(lights, 0, { 0, 0, 0 }, up)[0], fading = kernel_local_light(lights, 1, { 0, 0, 0 }, up)[0];
        const float under_spot = kernel_local_light(lights, 2, { 0, 0, 0 }, up)[0], beside_spot = kernel_local_light(lights, 2, { 2, 0, 0 }, up)[0];
        // Between the cones: 22.5 degrees off the axis, 2 / cos 22.5 away.
        const float offset = 2 * std::tan(0.3926991f), d = 2 / std::cos(0.3926991f), x = d / 8;
        const float between = kernel_local_light(lights, 2, { offset, 0, 0 }, up)[0];
        const float ramp = (std::cos(0.3926991f) - 0.8660254f) / (0.9659258f - 0.8660254f);
        check("the measured law: 2^2.2 / (1 + 25 x^2) at x = 0.25, with the fade at 7.5 of 8 m, a spot full inside its inner cone, 0 past its outer, "
              "and a linear ramp in the cosine between",
            near(full, power / (1 + 25 * 0.0625f), 1e-4f) && near(fading, power / (1 + 25 * 0.87890625f) * 0.3125f, 1e-4f)
                && near(under_spot, full, 1e-4f) && beside_spot == 0.0f && near(between, power * std::cos(0.3926991f) * ramp / (1 + 25 * x * x), 1e-4f));
    }
    {
        // Bounces: a pixel on the floor of a closed box whose walls of albedo 0.5 each give off 0.2. The light after n hits is
        // 0.2 (1 + 0.5 + ... + 0.5^(n-1)), and random ends of faded paths must leave that sum as it is.
        const uint32_t half = pack_light({ 0.5f, 0.5f, 0.5f }, AlbedoSource::direct);
        std::vector<TraceTriangle> box;
        quad(box, { -9, -9, -9 }, { -9, -9, 9 }, { 9, -9, 9 }, { 9, -9, -9 }, half);
        quad(box, { -9, 9, -9 }, { 9, 9, -9 }, { 9, 9, 9 }, { -9, 9, 9 }, half);
        quad(box, { -9, -9, -9 }, { -9, 9, -9 }, { -9, 9, 9 }, { -9, -9, 9 }, half);
        quad(box, { 9, -9, -9 }, { 9, -9, 9 }, { 9, 9, 9 }, { 9, 9, -9 }, half);
        quad(box, { -9, -9, -9 }, { 9, -9, -9 }, { 9, 9, -9 }, { -9, 9, -9 }, half);
        quad(box, { -9, -9, 9 }, { -9, 9, 9 }, { 9, 9, 9 }, { 9, -9, 9 }, half);
        Bvh closed = build_bvh(box);
        const float glow = 0.2f;
        for (BvhTriangle &t : closed.triangles)
            t.pad1 = pack_baked_light({ glow, glow, glow });
        const Frame inside = cast(closed, { 0, 0, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 16, 16 });
        const TraceConstants view = *trace_constants(TraceSettings {}, 0.6f, inside.camera, true, inside.image.size, { 0, 0, 0 }, { 0, 1, 0 }, 1);
        const auto mean_after = [&](uint32_t bounces, uint32_t &longest) {
            GatherConstants g = constants({ 0, 0, 0 }, { 0, 1, 0 }, 0, 4096);
            g.counts[1] = bounces;
            longest = 0;
            std::array<float, 4> history {}, hit {}, sky_sum {};
            const std::vector<float> black = even_sky(0);
            for (uint32_t i = 0; i < 4096; i++)
            {
                const GatherPixel p = gather_pixel(closed, inside.image, view, g, black, 8, 8, history, hit, sky_sum);
                history = p.history;
                hit = p.hit;
                sky_sum = p.sky;
                longest = std::max(longest, p.counters[static_cast<size_t>(GatherCounter::rays)]);
            }
            return hit[0] / 4096;
        };
        uint32_t rays_1 = 0, rays_2 = 0, rays_15 = 0;
        const float once = mean_after(1, rays_1), twice = mean_after(2, rays_2), most = mean_after(15, rays_15);
        check("one bounce brings the first hit's light on one ray", near(once, 0.2f, 0.002f) && rays_1 == 1);
        check("two bring half as much again on two rays", near(twice, 0.3f, 0.004f) && rays_2 == 2);
        check("15 bring nearly twice the first, past 2 rays on some paths and within 15", near(most, 0.4f * (1 - 1.0f / 32768), 0.008f) && rays_15 > 2 && rays_15 <= 15);
        // With a face's picture over the floor: at a hit that is not the last, the picture's ambient light is left out for what
        // the next hit brings, but all of it stays at the last.
        GatherConstants lit = constants({ 0, 0, 0 }, { 0, 1, 0 }, 0.4f, 1);
        lit.ambient[0][3] = 1;
        const float kept = gather_pixel(closed, inside.image, view, lit, even_sky(0), 8, 8, {}, {}, {}).light[0];
        check("the box's walls, no face: the last hit keeps its ambient light", near(kept, 0.2f + 0.5f * 0.4f, 0.002f));
        // Bounce strength 2 brightens what the walls reflect, but no albedo past 0.9: 1.8 times here.
        GatherSettings stronger;
        stronger.samples = 1;
        stronger.strength = 2;
        const GatherConstants g2 = gather_constants(stronger, SunLight {}, AmbientFit {});
        const float brighter = gather_pixel(closed, inside.image, view, g2, even_sky(0), 8, 8, {}, {}, {}).light[0];
        check("Bounce strength 2 has the walls reflect 1.8 times their light, to an albedo of 0.9, and applies the bounce at 1",
            near(brighter, 0.2f * 1.8f, 0.004f) && gather_apply_constants(stronger, {}, GatherView::none, 64, 1).apply[0] == 1
            && gather_apply_constants(GatherSettings {}, {}, GatherView::none, 64, 1).apply[0] == 1);
    }
    {
        // Reflections: a wall's pixel with stored smoothness 0.5 under an even sky, and a card in its mirror direction.
        TraceImage shiny = wall;
        shiny.smoothness.assign(64, 128);
        shiny.material.assign(64, 0);
        const uint32_t white = pack_light({ 1, 1, 1 }, AlbedoSource::direct);
        std::vector<TraceTriangle> far_away;
        quad(far_away, { -900, -900, -900 }, { -900, -900, -899 }, { -899, -900, -899 }, { -899, -900, -900 }, white);
        const Bvh open = build_bvh(far_away);
        const GatherConstants dark = constants({ 0, 0, 0 }, sun, 0, 64);
        const std::vector<float> even = even_sky(0.7f);
        ReflectSettings settings;
        settings.enabled = true;
        settings.gloss = 0.5f;
        settings.reflected_light = 16;
        const ReflectConstants half = reflect_constants(settings);
        check("the constants: roughness times 1 - gloss, and the light sources' gain; none without reflections", half.params[0] == 0.5f && half.params[1] == 16
            && reflect_constants(ReflectSettings {}).params[1] == 0);
        std::array<float, 4> sum {};
        bool even_light = true;
        for (int i = 0; i < 64; i++)
        {
            const ReflectPixel p = reflect_pixel(open, shiny, c, dark, half, even, 3, 3, sum);
            even_light = even_light && p.world && p.rays == 1;
            sum = p.sum;
        }
        check("under an even sky each sample of 64 brings its light, one ray each", even_light && near(sum[0] / sum[3], 0.7f, 1e-4f) && sum[3] == 64);
        TraceImage snow = shiny;
        snow.material.assign(64, 19);
        check("a material whose G-buffer holds something else keeps the game's reflections", !reflect_pixel(open, snow, c, dark, half, even, 3, 3, {}).world);
        // The wall faces +x at x = 0, the camera 2 m out: a pixel's mirror ray leaves toward +x. A card at x = 3 across its path.
        settings.gloss = 1;
        const ReflectConstants mirror = reflect_constants(settings);
        std::vector<TraceTriangle> card;
        quad(card, { 3, -100, -100 }, { 3, 100, -100 }, { 3, 100, 100 }, { 3, -100, 100 }, white);
        const Bvh ahead = build_bvh(card);
        GatherConstants lit = constants({ 0, 0, 0 }, sun, 0, 64);
        for (int k = 0; k < 3; k++)
            lit.ambient[0][k] = 0.25f;
        std::array<float, 4> hits {};
        for (int i = 0; i < 16; i++)
            hits = reflect_pixel(ahead, shiny, c, lit, mirror, even, 3, 3, hits).sum;
        check("a gloss of 1 mirrors: every sample meets the card ahead and brings its ambient light, not the sky's", near(hits[0] / hits[3], 0.25f, 1e-3f));
        check("light sources: none below the mark, all of the gain from twice it, and a smooth step between", kernel_source_gain(0.5f, 16, 1) == 1 && kernel_source_gain(2, 16, 1) == 16
            && near(kernel_source_gain(1.5f, 16, 1), 8.5f, 1e-4f) && kernel_source_gain(5, 1, 1) == 1 && kernel_source_gain(5, 16, 0) == 1);
        check("gloss: 0 keeps the stored roughness, 0.5 halves it with a floor of 0.12, and 1 mirrors", kernel_reflect_roughness(0.4f, 1) == 0.6f
            && near(kernel_reflect_roughness(0.2f, 0.5f), 0.4f, 1e-6f) && near(kernel_reflect_roughness(0.9f, 0.5f), 0.12f, 1e-6f) && kernel_reflect_roughness(0.4f, 0) == 0);
        const TraceKey plain = trace_key(1, { 8, 8 }, side, sun, TraceSettings {}, &dark, {}, half);
        check("reflections that change start the samples again", trace_reset(plain, trace_key(1, { 8, 8 }, side, sun, TraceSettings {}, &dark, {}, mirror))
            && !trace_reset(plain, trace_key(1, { 8, 8 }, side, sun, TraceSettings {}, &dark, {}, half)));
    }
    std::printf("%s: %d failed\n", __FILE__, failures);
    return failures == 0 ? 0 : 1;
}
