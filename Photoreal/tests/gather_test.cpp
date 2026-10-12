// The gather kernel compiled as C++ (shaders/gather.hlsli through core/kernel.cpp): a white furnace, a wall above lit
// ground, the same ground under a roof, a foliage card lit from behind, the energy bound over a courtyard, the gather's
// occlusion sample against the trace kernel's for the same pixel and index, its counters, and the gather's constants.
#include "../core/trace.h"
#include "../photoreal.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace photoreal;

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
        uint32_t samples, std::vector<std::array<float, 4>> *lights = nullptr)
    {
        Sums s;
        for (uint32_t i = 0; i < samples; i++)
        {
            const GatherPixel p = gather_pixel(bvh, image, c, g, sky, x, y, s.history, s.hit, s.sky);
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
        check("the gather's constants: the sun's color, the shading direction made unit, the ambient rows times hit_ambient, 128 samples",
            k.sun[0] == 2.8f && k.sun[2] == 1.1f && k.shade[1] == 1 && k.ambient[0][2] == 0.3f && k.ambient[2][1] == 0.1f && k.counts[0] == 128);
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
        TraceSettings t;
        GatherSettings gs;
        check("the target: the larger of the gather's and the sun's samples while gathering, else of the occlusion's and the sun's",
            trace_target(t, gs, true) == 128 && trace_target(t, gs, false) == 64 && (gs.samples = 16, trace_target(t, gs, true)) == 32);
        check("the trace kernel runs beside the gather while the sun has samples to take, then every 8th pass",
            trace_kernel_runs(0, 32) && trace_kernel_runs(31, 32) && trace_kernel_runs(32, 32) && !trace_kernel_runs(33, 32) && !trace_kernel_runs(39, 32)
                && trace_kernel_runs(40, 32));
        check("the error: 1.25 x deviations / (means x sqrt(samples))", near(float(gather_error(1000, 10000, 64)), 0.015625f, 1e-7f) && gather_error(5, 0, 64) == 0);
    }
    std::printf("%s: %d failed\n", __FILE__, failures);
    return failures == 0 ? 0 : 1;
}
