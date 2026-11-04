#include "trace.h"
#include "../enhancements.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <thread>

namespace enhancements
{
    namespace
    {
        // Unity's column-major memory order into ours.
        Mat4d from_unity(const float m[16])
        {
            Mat4d out {};
            for (int r = 0; r < 4; r++)
                for (int c = 0; c < 4; c++)
                    out[r * 4 + c] = m[c * 4 + r];
            return out;
        }

        Mat4d multiply(const Mat4d &a, const Mat4d &b)
        {
            Mat4d out {};
            for (int r = 0; r < 4; r++)
                for (int c = 0; c < 4; c++)
                    for (int k = 0; k < 4; k++)
                        out[r * 4 + c] += a[r * 4 + k] * b[k * 4 + c];
            return out;
        }

        // Every other bit of v, from bit 0, packed: a Morton code's x.
        uint32_t compact(uint32_t v)
        {
            v &= 0x55555555u;
            v = (v | (v >> 1)) & 0x33333333u;
            v = (v | (v >> 2)) & 0x0F0F0F0Fu;
            v = (v | (v >> 4)) & 0x00FF00FFu;
            v = (v | (v >> 8)) & 0x0000FFFFu;
            return v;
        }

        float largest(const std::array<float, 16> &m)
        {
            float most = 0;
            for (float v : m)
                most = std::max(most, std::fabs(v));
            return most;
        }

        bool matrix_moved(const std::array<float, 16> &a, const std::array<float, 16> &b)
        {
            const float tolerance = 1e-6f * std::max(largest(a), largest(b));
            for (size_t i = 0; i < a.size(); i++)
                if (!(std::fabs(a[i] - b[i]) <= tolerance))
                    return true;
            return false;
        }

        Row row(const SunCbuffers &bytes, uint32_t index)
        {
            Row r;
            std::memcpy(r.data(), bytes.data() + index * 16, sizeof r);
            return r;
        }

        // The buffers' first rows in SunCbuffers, in rows of 16 bytes, as core/sun.cpp has them.
        constexpr uint32_t kB1 = 768 / 16, kB3 = (768 + 192 + 352) / 16;

        // Where TraceKey::light holds the gather's sun color, its shading direction and its four ambient rows.
        constexpr size_t kKeySun = 0, kKeyShade = 3, kKeyAmbient = 6;
    }

    uint32_t caster_classes(const TraceSettings &settings)
    {
        const auto bit = [](TraceClass c) { return 1u << static_cast<uint32_t>(c); };
        uint32_t classes = bit(TraceClass::world) | bit(TraceClass::other);
        if (settings.alpha_cards)
            classes |= bit(TraceClass::vegetation) | bit(TraceClass::foliage);
        if (settings.characters)
            classes |= bit(TraceClass::character);
        return settings.whole_cards ? classes : classes | kAlphaTests;
    }

    Vec3d camera_position(const Camera &camera)
    {
        const std::optional<Mat4d> view_to_world = invert(from_unity(camera.world_to_view));
        if (!view_to_world)
            return { NAN, NAN, NAN };
        return { (*view_to_world)[3], (*view_to_world)[7], (*view_to_world)[11] };
    }

    std::optional<TraceConstants> trace_constants(const TraceSettings &settings, float level, const Camera &camera, bool flip, Size render,
        const Vec3d &reference, const Vec3 &toward_sun, uint32_t seed)
    {
        const std::optional<Mat4d> view_to_world = invert(from_unity(camera.world_to_view));
        const std::optional<Mat4d> clip_to_view = invert(from_unity(camera.view_to_clip));
        if (!view_to_world || !clip_to_view)
            return std::nullopt;
        Mat4d to_scene = { 1, 0, 0, -reference[0], 0, 1, 0, -reference[1], 0, 0, 1, -reference[2], 0, 0, 0, 1 };
        const Mat4d clip_to_scene = multiply(to_scene, multiply(*view_to_world, *clip_to_view));
        TraceConstants c {};
        for (int r = 0; r < 4; r++)
            for (int k = 0; k < 4; k++)
            {
                c.clip_rows[r][k] = static_cast<float>(clip_to_scene[r * 4 + k]);
                if (!std::isfinite(c.clip_rows[r][k]))
                    return std::nullopt;
            }
        for (int k = 0; k < 3; k++)
            c.eye_position[k] = static_cast<float>((*view_to_world)[k * 4 + 3] - reference[k]);
        const double length = std::sqrt(double(toward_sun[0]) * toward_sun[0] + double(toward_sun[1]) * toward_sun[1] + double(toward_sun[2]) * toward_sun[2]);
        if (std::isfinite(length) && length > 0)
        {
            for (int k = 0; k < 3; k++)
                c.sun_cone[k] = static_cast<float>(toward_sun[k] / length);
            // The cone's half angle has tangent sun_size / 2.
            c.sun_cone[3] = static_cast<float>(1 / std::sqrt(1 + 0.25 * double(settings.sun_size) * settings.sun_size));
        }
        c.trace_params[0] = settings.ao_distance;
        c.trace_params[1] = settings.ao_strength;
        c.trace_params[2] = settings.sun_strength;
        c.trace_params[3] = level;
        c.trace_counts[0] = settings.ao_samples;
        c.trace_counts[1] = settings.sun_samples;
        c.trace_counts[2] = caster_classes(settings);
        c.trace_counts[3] = (settings.ao ? ENHANCEMENTS_TRACE_AO : 0u) | (settings.sun ? ENHANCEMENTS_TRACE_SUN : 0u);
        c.trace_tiles[3] = seed;
        c.trace_render[0] = render.width;
        c.trace_render[1] = render.height;
        c.trace_render[2] = flip ? 1 : 0;
        c.trace_render[3] = kGridStride;
        return c;
    }

    GatherConstants gather_constants(const GatherSettings &settings, const SunLight &sun, const AmbientFit &ambient, float faces_ambient)
    {
        GatherConstants g {};
        const double length = std::sqrt(double(sun.shade[0]) * sun.shade[0] + double(sun.shade[1]) * sun.shade[1] + double(sun.shade[2]) * sun.shade[2]);
        if (std::isfinite(length) && length > 0)
            for (int k = 0; k < 3; k++)
            {
                g.sun[k] = sun.color[k];
                g.shade[k] = static_cast<float>(sun.shade[k] / length);
            }
        for (int r = 0; r < 4; r++)
            for (int k = 0; k < 3; k++)
                g.ambient[r][k] = ambient.rows[r][k] * settings.hit_ambient;
        if (settings.hit_ambient > 0)
            g.ambient[0][3] = faces_ambient / settings.hit_ambient;
        g.ambient[1][3] = std::max(settings.strength, 1.0f);
        g.counts[0] = settings.samples;
        g.counts[1] = settings.bounces;
        return g;
    }

    bool TraceLight::operator==(const TraceLight &o) const
    {
        return position == o.position && range == o.range && radiance == o.radiance && cos_outer == o.cos_outer && direction == o.direction
            && cos_inner == o.cos_inner && shadows == o.shadows;
    }

    std::vector<TraceLight> parse_trace_lights(const ::EnhancementsShotLight *lights, uint32_t count, uint32_t max)
    {
        std::vector<TraceLight> out;
        if (lights == nullptr)
            return out;
        const auto finite = [](float v) { return std::isfinite(v); };
        for (uint32_t i = 0; i < count && out.size() < max; i++)
        {
            const ::EnhancementsShotLight &raw = lights[i];
            TraceLight l;
            if (!finite(raw.position[0]) || !finite(raw.position[1]) || !finite(raw.position[2]) || !finite(raw.range) || !(raw.range > 0))
                continue;
            l.position = { raw.position[0], raw.position[1], raw.position[2] };
            l.range = std::min(raw.range, 1000.0f);
            for (int k = 0; k < 3; k++)
                l.radiance[k] = finite(raw.color[k]) && raw.color[k] > 0 ? std::pow(raw.color[k], kLocalLightPower) : 0.0f;
            if (l.radiance[0] + l.radiance[1] + l.radiance[2] <= 0)
                continue;
            const double length = std::sqrt(double(raw.direction[0]) * raw.direction[0] + double(raw.direction[1]) * raw.direction[1]
                + double(raw.direction[2]) * raw.direction[2]);
            const bool spot = finite(raw.cos_outer) && raw.cos_outer >= -1 && std::isfinite(length) && length > 0;
            if (spot)
            {
                for (int k = 0; k < 3; k++)
                    l.direction[k] = static_cast<float>(raw.direction[k] / length);
                l.cos_outer = std::min(raw.cos_outer, 1.0f);
                l.cos_inner = finite(raw.cos_inner) ? std::clamp(raw.cos_inner, l.cos_outer, 1.0f) : l.cos_outer;
            }
            l.shadows = (raw.reserved[0] & ENHANCEMENTS_SHOT_LIGHT_SHADOWS) != 0;
            out.push_back(l);
        }
        return out;
    }

    static_assert(ENHANCEMENTS_MAX_SHOT_LIGHTS == 4);

    ReflectConstants reflect_constants(const ReflectSettings &settings)
    {
        ReflectConstants c;
        if (settings.enabled)
        {
            c.params[0] = 1 - settings.gloss;
            c.params[1] = settings.reflected_light;
        }
        return c;
    }

    uint32_t pack_baked_light(const Vec3 &radiance)
    {
        uint32_t word = 0;
        for (int k = 0; k < 3; k++)
        {
            const float v = std::sqrt(std::clamp(radiance[k] / kBakedLightRange, 0.0f, 1.0f));
            word |= static_cast<uint32_t>(v * 1023.0f + 0.5f) << (10 * k);
        }
        return word;
    }

    float light_geometry(const TraceLight &light, const Vec3d &p, const Vec3d &n)
    {
        const Vec3d v = { light.position[0] - p[0], light.position[1] - p[1], light.position[2] - p[2] };
        const double d = std::max(std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]), 1e-3);
        const double x = d / light.range;
        if (x >= 1)
            return 0;
        const double cosine = (n[0] * v[0] + n[1] * v[1] + n[2] * v[2]) / d;
        if (cosine <= 0)
            return 0;
        const double along = -(light.direction[0] * v[0] + light.direction[1] * v[1] + light.direction[2] * v[2]) / d;
        const double spot = std::clamp((along - light.cos_outer) / std::max<double>(light.cos_inner - light.cos_outer, 1e-4), 0.0, 1.0);
        return static_cast<float>(cosine * std::clamp((1 - x) * 5, 0.0, 1.0) * spot / (1 + 25 * x * x));
    }

    GameLightCounts bake_game_lights(Bvh &bvh, const std::vector<TraceLight> &lights, const Vec3d &reference, const std::vector<uint32_t> *alpha, uint32_t tasks)
    {
        constexpr uint32_t kCasters = (1u << static_cast<uint32_t>(TraceClass::world)) | (1u << static_cast<uint32_t>(TraceClass::vegetation))
            | (1u << static_cast<uint32_t>(TraceClass::foliage)) | (1u << static_cast<uint32_t>(TraceClass::other)) | kAlphaTests;
        const size_t count = bvh.triangles.size();
        std::atomic<uint64_t> lit { 0 }, rays { 0 };
        const auto work = [&](size_t first, size_t last) {
            uint64_t my_lit = 0, my_rays = 0;
            for (size_t i = first; i < last; i++)
            {
                BvhTriangle &t = bvh.triangles[i];
                t.pad1 = 0;
                const Vec3d e1 = { t.e1[0], t.e1[1], t.e1[2] }, e2 = { t.e2[0], t.e2[1], t.e2[2] };
                Vec3d n = { e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0] };
                const double length = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
                if (!(length > 0))
                    continue;
                n = { n[0] / length, n[1] / length, n[2] / length };
                const Vec3d local = { t.v0[0] + (e1[0] + e2[0]) / 3, t.v0[1] + (e1[1] + e2[1]) / 3, t.v0[2] + (e1[2] + e2[2]) / 3 };
                const Vec3d world = { local[0] + reference[0], local[1] + reference[1], local[2] + reference[2] };
                Vec3 sum {};
                for (const TraceLight &light : lights)
                {
                    const float geometry = light_geometry(light, world, n);
                    if (!(geometry > 1e-4f))
                        continue;
                    if (light.shadows)
                    {
                        const Vec3d v = { light.position[0] - world[0], light.position[1] - world[1], light.position[2] - world[2] };
                        const double d = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
                        const double reach = d - kGameLightClearance;
                        if (reach > 0.02)
                        {
                            my_rays++;
                            const Vec3f origin = { float(local[0] + n[0] * 0.02), float(local[1] + n[1] * 0.02), float(local[2] + n[2] * 0.02) };
                            if (trace_any(bvh, origin, { float(v[0] / d), float(v[1] / d), float(v[2] / d) }, float(reach), kCasters, nullptr, alpha))
                                continue;
                        }
                    }
                    for (int k = 0; k < 3; k++)
                        sum[k] += light.radiance[k] * geometry;
                }
                if (sum[0] + sum[1] + sum[2] <= 0)
                    continue;
                const Vec3f albedo = light_albedo(t.light);
                t.pad1 = pack_baked_light({ sum[0] * albedo[0], sum[1] * albedo[1], sum[2] * albedo[2] });
                my_lit++;
            }
            lit += my_lit;
            rays += my_rays;
        };
        std::vector<std::thread> threads;
        tasks = std::max<uint32_t>(1, tasks);
        for (uint32_t k = 1; k < tasks; k++)
            threads.emplace_back(work, count * k / tasks, count * (k + 1) / tasks);
        work(0, count / tasks);
        for (std::thread &thread : threads)
            thread.join();
        return { lit.load(), rays.load() };
    }

    GatherLights gather_lights(const std::vector<TraceLight> &lights, const Vec3d &reference)
    {
        GatherLights g {};
        const uint32_t count = static_cast<uint32_t>(std::min<size_t>(lights.size(), ENHANCEMENTS_MAX_SHOT_LIGHTS));
        for (uint32_t i = 0; i < count; i++)
        {
            const TraceLight &l = lights[i];
            for (int k = 0; k < 3; k++)
            {
                g.rows[i][0][k] = static_cast<float>(l.position[k] - reference[k]);
                g.rows[i][1][k] = l.radiance[k];
                g.rows[i][2][k] = l.direction[k];
            }
            g.rows[i][0][3] = l.range;
            g.rows[i][1][3] = l.cos_outer;
            g.rows[i][2][3] = l.cos_inner;
            g.rows[i][3][1] = 0.2126f * l.radiance[0] + 0.7152f * l.radiance[1] + 0.0722f * l.radiance[2];
        }
        g.info[0] = count;
        return g;
    }

    GatherFaces gather_faces(const std::vector<FaceLight> &faces)
    {
        GatherFaces g {};
        const uint32_t count = static_cast<uint32_t>(std::min<size_t>(faces.size(), kFaceCount));
        for (uint32_t i = 0; i < count; i++)
        {
            std::memcpy(g.rows[i], faces[i].scene_to_clip, sizeof g.rows[i]);
            std::memcpy(g.w[i], faces[i].depth_to_w, sizeof g.w[i]);
        }
        if (count != 0)
        {
            g.info[0] = count;
            g.info[1] = faces[0].size.width;
            g.info[2] = faces[0].size.height;
        }
        return g;
    }

    GatherView gather_view(View view)
    {
        switch (view)
        {
        case View::bounce: return GatherView::bounce;
        case View::sky_visibility: return GatherView::sky_visibility;
        case View::sky_map: return GatherView::sky_map;
        default: return GatherView::none;
        }
    }

    uint32_t gather_filter_passes(const GatherSettings &settings, uint32_t samples)
    {
        return std::max<uint32_t>(settings.filter, samples < kGatherBlurBelow ? 1 : 0);
    }

    GatherApplyConstants gather_apply_constants(const GatherSettings &settings, const std::array<Vec3f, 9> &harmonics, GatherView view, uint32_t samples,
        uint32_t step)
    {
        GatherApplyConstants c {};
        c.apply[0] = std::min(settings.strength, 1.0f);
        c.apply[1] = static_cast<float>(step);
        c.modes[0] = static_cast<uint32_t>(settings.sky);
        c.modes[1] = static_cast<uint32_t>(view);
        c.modes[2] = samples < kGatherBlurBelow ? 1 : 0;
        for (int i = 0; i < 9; i++)
            for (int k = 0; k < 3; k++)
                c.harmonics[i][k] = harmonics[i][k];
        return c;
    }

    std::vector<uint32_t> tile_order(Size render)
    {
        const uint32_t columns = (render.width + kTileSize - 1) / kTileSize, rows = (render.height + kTileSize - 1) / kTileSize;
        uint32_t side = 1;
        while (side < std::max(columns, rows))
            side *= 2;
        std::vector<uint32_t> order;
        order.reserve(size_t(columns) * rows);
        for (uint32_t code = 0; code < side * side; code++)
        {
            const uint32_t x = compact(code), y = compact(code >> 1);
            if (x < columns && y < rows)
                order.push_back(x | y << 16);
        }
        return order;
    }

    std::optional<TraceDispatch> trace_next(TraceSchedule &s, uint32_t samples)
    {
        const uint64_t target = uint64_t(s.tiles) * samples;
        if (s.tiles == 0 || s.visits >= target)
            return std::nullopt;
        TraceDispatch d;
        d.first = static_cast<uint32_t>(s.visits % s.tiles);
        d.count = static_cast<uint32_t>(std::min<uint64_t>({ std::max<uint64_t>(s.batch, 1), target - s.visits, s.tiles }));
        s.visits += d.count;
        return d;
    }

    std::vector<TraceDispatch> trace_chunks(const TraceDispatch &frame, uint32_t tiles, uint32_t chunk)
    {
        std::vector<TraceDispatch> out;
        chunk = std::max<uint32_t>(chunk, 1);
        for (uint32_t done = 0; done < frame.count && tiles > 0; done += chunk)
            out.push_back({ static_cast<uint32_t>((uint64_t(frame.first) + done) % tiles), std::min(chunk, frame.count - done) });
        return out;
    }

    uint32_t next_batch(uint32_t batch, uint32_t measured, double measured_ms, double budget_ms, uint32_t tiles)
    {
        const double most = std::min<double>(2.0 * std::max<uint32_t>(batch, 1), std::max<uint32_t>(tiles, 1));
        const double wanted = measured_ms > 0 ? std::max<uint32_t>(measured, 1) * budget_ms / measured_ms : most;
        return static_cast<uint32_t>(std::clamp(std::floor(wanted), 1.0, most));
    }

    uint32_t next_chunk(uint32_t measured, double measured_ms)
    {
        if (!(measured_ms > 0))
            return 1;
        const double wanted = std::max<uint32_t>(measured, 1) * kSubmissionMs / measured_ms;
        return static_cast<uint32_t>(std::clamp(std::floor(wanted), 1.0, double(kMaxChunkTiles)));
    }

    TraceKey trace_key(uint64_t generation, Size render, const Camera &camera, const Vec3 &toward_sun, const TraceSettings &settings,
        const GatherConstants *gather, const GatherLights &lights, const ReflectConstants &reflect)
    {
        TraceKey k;
        k.lights = lights;
        k.generation = generation;
        k.render = render;
        std::memcpy(k.world_to_view.data(), camera.world_to_view, sizeof camera.world_to_view);
        std::memcpy(k.view_to_clip.data(), camera.view_to_clip, sizeof camera.view_to_clip);
        k.toward_sun = toward_sun;
        k.ao_distance = settings.ao_distance;
        k.sun_size = settings.sun_size;
        k.casters = caster_classes(settings);
        if (gather)
        {
            k.gather = true;
            for (int i = 0; i < 3; i++)
            {
                k.light[kKeySun + i] = gather->sun[i];
                k.light[kKeyShade + i] = gather->shade[i];
                for (int r = 0; r < 4; r++)
                    k.light[kKeyAmbient + r * 3 + i] = gather->ambient[r][i];
            }
            k.bounces = gather->counts[1];
            k.faces_ambient = gather->ambient[0][3];
            k.surface = gather->ambient[1][3];
        }
        k.reflect = reflect;
        return k;
    }

    static_assert(ENHANCEMENTS_TRACE_RESTART_SCENE == static_cast<int>(TraceRestart::scene) && ENHANCEMENTS_TRACE_RESTART_LIGHT == static_cast<int>(TraceRestart::light)
        && ENHANCEMENTS_TRACE_RESTART_COUNT == static_cast<int>(TraceRestart::count));

    const std::array<const char *, static_cast<size_t>(TraceRestart::count)> kTraceRestartNames = { "none", "scene", "render size", "settings", "camera",
        "sun direction", "light" };

    TraceRestart trace_restart(const TraceKey &a, const TraceKey &b)
    {
        const auto apart = [&](size_t first, size_t count) {
            for (size_t i = first; i < first + count; i++)
                if (!(std::fabs(a.light[i] - b.light[i]) <= 1e-5f))
                    return true;
            return false;
        };
        if (a.generation != b.generation)
            return TraceRestart::scene;
        if (a.render != b.render)
            return TraceRestart::render;
        if (a.ao_distance != b.ao_distance || a.sun_size != b.sun_size || a.casters != b.casters || a.gather != b.gather || a.bounces != b.bounces || a.faces_ambient != b.faces_ambient || a.surface != b.surface || std::memcmp(a.reflect.params, b.reflect.params, sizeof a.reflect.params) != 0 || apart(kKeyAmbient, 12))
            return TraceRestart::settings;
        if (matrix_moved(a.world_to_view, b.world_to_view) || matrix_moved(a.view_to_clip, b.view_to_clip))
            return TraceRestart::camera;
        for (int k = 0; k < 3; k++)
            if (!(std::fabs(a.toward_sun[k] - b.toward_sun[k]) <= 1e-5f))
                return TraceRestart::sun_direction;
        if (apart(kKeyShade, 3))
            return TraceRestart::sun_direction;
        return apart(kKeySun, 3) || std::memcmp(&a.lights, &b.lights, sizeof a.lights) != 0 ? TraceRestart::light : TraceRestart::none;
    }

    std::string restart_text(TraceRestart reason, const TraceKey *before, const TraceKey &b)
    {
        if (!before)
            return "a scene's first samples";
        const TraceKey &a = *before;
        char text[320];
        const auto vec = [](const float *v) {
            char out[64];
            std::snprintf(out, sizeof out, "(%.6g, %.6g, %.6g)", v[0], v[1], v[2]);
            return std::string(out);
        };
        const auto went = [&](const char *what, const float *from, const float *to, bool direction) {
            std::string out = std::string(what) + " went from " + vec(from) + " to " + vec(to);
            if (direction)
            {
                const double cosine = double(from[0]) * to[0] + double(from[1]) * to[1] + double(from[2]) * to[2];
                char degrees[48];
                std::snprintf(degrees, sizeof degrees, ", %.4f degrees", std::acos(std::clamp(cosine, -1.0, 1.0)) * 57.29577951308232);
                out += degrees;
            }
            return out;
        };
        const auto farthest = [](const std::array<float, 16> &x, const std::array<float, 16> &y) {
            float most = 0;
            for (size_t i = 0; i < x.size(); i++)
                most = std::max(most, std::fabs(x[i] - y[i]));
            return most;
        };
        switch (reason)
        {
        case TraceRestart::scene:
            std::snprintf(text, sizeof text, "the scene went from generation %llu to %llu", static_cast<unsigned long long>(a.generation),
                static_cast<unsigned long long>(b.generation));
            return text;
        case TraceRestart::render:
            std::snprintf(text, sizeof text, "the render size went from %ux%u to %ux%u", a.render.width, a.render.height, b.render.width, b.render.height);
            return text;
        case TraceRestart::settings:
            if (a.gather != b.gather)
                return b.gather ? "the gather went on" : "the gather went off";
            if (a.ao_distance != b.ao_distance)
                std::snprintf(text, sizeof text, "the occlusion distance went from %g to %g", a.ao_distance, b.ao_distance);
            else if (a.sun_size != b.sun_size)
                std::snprintf(text, sizeof text, "the sun's size went from %g to %g", a.sun_size, b.sun_size);
            else if (a.casters != b.casters)
                std::snprintf(text, sizeof text, "the casters went from 0x%X to 0x%X", a.casters, b.casters);
            else if (a.bounces != b.bounces)
                std::snprintf(text, sizeof text, "the bounces went from %u to %u", a.bounces, b.bounces);
            else if (a.surface != b.surface)
                std::snprintf(text, sizeof text, "the bounce strength's surface gain went from %g to %g", a.surface, b.surface);
            else
                return went("the ambient light at a hit, its first row,", &a.light[kKeyAmbient], &b.light[kKeyAmbient], false);
            return text;
        case TraceRestart::camera:
            std::snprintf(text, sizeof text, "the camera moved: its view by up to %.3g an element and its projection by up to %.3g",
                farthest(a.world_to_view, b.world_to_view), farthest(a.view_to_clip, b.view_to_clip));
            return text;
        case TraceRestart::sun_direction:
            for (int k = 0; k < 3; k++)
                if (!(std::fabs(a.toward_sun[k] - b.toward_sun[k]) <= 1e-5f))
                    return went("the direction toward the sun", a.toward_sun.data(), b.toward_sun.data(), true);
            return went("the sun's shading direction", &a.light[kKeyShade], &b.light[kKeyShade], true);
        case TraceRestart::light:
            return went("the sun's color", &a.light[kKeySun], &b.light[kKeySun], false);
        case TraceRestart::none:
        case TraceRestart::count:
            break;
        }
        return "nothing changed";
    }

    bool restart_logged(RestartBurst &burst, TraceRestart reason)
    {
        const uint32_t bit = 1u << static_cast<uint32_t>(reason);
        const bool first = (burst.named & bit) == 0;
        burst.named |= bit;
        burst.count++;
        return first || (burst.count >= kRestartLogFrom && (burst.count & (burst.count - 1)) == 0);
    }

    TraceConstants held_constants(const TraceConstants &start, const TraceConstants &now)
    {
        TraceConstants c = now;
        std::memcpy(c.clip_rows, start.clip_rows, sizeof c.clip_rows);
        std::memcpy(c.eye_position, start.eye_position, sizeof c.eye_position);
        std::memcpy(c.sun_cone, start.sun_cone, sizeof c.sun_cone);
        c.trace_params[0] = start.trace_params[0];
        c.trace_counts[2] = start.trace_counts[2];
        return c;
    }

    GatherConstants held_gather(const GatherConstants &start, const GatherConstants &now)
    {
        GatherConstants c = start;
        std::memcpy(c.counts, now.counts, sizeof c.counts);
        return c;
    }

    SunFacts sun_facts(const SunCbuffers &bytes)
    {
        SunFacts facts;
        const Row origin = row(bytes, kB1 + 10);
        if (std::isfinite(origin[0]) && std::isfinite(origin[1]) && std::isfinite(origin[2]))
            facts.floating_origin = Vec3 { origin[0], origin[1], origin[2] };
        const double g[3] = { row(bytes, kB3 + 15)[2], row(bytes, kB3 + 16)[2], row(bytes, kB3 + 17)[2] };
        const double length = std::sqrt(g[0] * g[0] + g[1] * g[1] + g[2] * g[2]);
        if (std::isfinite(length) && length > 0)
            facts.toward_sun = Vec3 { float(g[0] / length), float(g[1] / length), float(g[2] / length) };
        return facts;
    }

    bool origin_moved(const Vec3 &a, const Vec3 &b)
    {
        const double dx = double(a[0]) - b[0], dy = double(a[1]) - b[1], dz = double(a[2]) - b[2];
        return !(dx * dx + dy * dy + dz * dz <= 1e-4);
    }

    std::string grid_breakdown(const TraceCounts &counts)
    {
        static const char *const kNames[] = { "stencil 128", "stencil 144", "other" };
        static_assert(std::size(kNames) == static_cast<size_t>(GridClass::count));
        std::string out;
        for (uint32_t c = 0; c < static_cast<uint32_t>(GridClass::count); c++)
        {
            const auto at = [&](GridSlot s) { return counts[grid_counter(static_cast<GridClass>(c), s)]; };
            const uint64_t off = at(GridSlot::no_hit) + at(GridSlot::nearer) + at(GridSlot::farther);
            out += std::string(out.empty() ? "" : ", ") + kNames[c] + " " + std::to_string(off) + " of " + std::to_string(at(GridSlot::tested)) + " off";
            if (off != 0)
                out += " (" + std::to_string(at(GridSlot::no_hit)) + " met nothing, " + std::to_string(at(GridSlot::nearer)) + " nearer, "
                    + std::to_string(at(GridSlot::farther)) + " farther)";
        }
        return out;
    }

    uint32_t trace_target(const TraceSettings &trace, const GatherSettings &gather, bool gathering)
    {
        return std::max(gathering ? gather.samples : trace.ao_samples, trace.sun_samples);
    }

    bool trace_kernel_runs(uint32_t passes_before, uint32_t sun_samples)
    {
        return passes_before < sun_samples || passes_before % kTraceGridPasses == 0;
    }

    double gather_error(uint64_t deviation, uint64_t mean, uint32_t samples)
    {
        return mean == 0 || samples == 0 ? 0 : 1.25 * double(deviation) / (double(mean) * std::sqrt(double(samples)));
    }

    bool consistency_stale(uint64_t tested, uint64_t off)
    {
        return tested >= 32 && off * 20 > tested;
    }

    bool shot_consistency_stale(uint64_t tested, uint64_t off)
    {
        return tested >= 32 && off * 2 > tested;
    }
}
