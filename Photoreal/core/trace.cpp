#include "trace.h"
#include "../photoreal.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace photoreal
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
    }

    uint32_t caster_classes(const TraceSettings &settings)
    {
        const auto bit = [](TraceClass c) { return 1u << static_cast<uint32_t>(c); };
        uint32_t classes = bit(TraceClass::world) | bit(TraceClass::other);
        if (settings.alpha_cards)
            classes |= bit(TraceClass::vegetation) | bit(TraceClass::foliage);
        if (settings.characters)
            classes |= bit(TraceClass::character);
        return classes;
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
        c.trace_counts[3] = (settings.ao ? PHOTOREAL_TRACE_AO : 0u) | (settings.sun ? PHOTOREAL_TRACE_SUN : 0u);
        c.trace_tiles[3] = seed;
        c.trace_render[0] = render.width;
        c.trace_render[1] = render.height;
        c.trace_render[2] = flip ? 1 : 0;
        c.trace_render[3] = kGridStride;
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

    uint32_t next_batch(uint32_t batch, double measured_ms, double budget_ms, uint32_t tiles)
    {
        const double most = std::min<double>(2.0 * std::max<uint32_t>(batch, 1), std::max<uint32_t>(tiles, 1));
        const double wanted = measured_ms > 0 ? batch * budget_ms / measured_ms : most;
        return static_cast<uint32_t>(std::clamp(std::floor(wanted), 1.0, most));
    }

    TraceKey trace_key(uint64_t generation, Size render, const Camera &camera, const Vec3 &toward_sun, const TraceSettings &settings)
    {
        TraceKey k;
        k.generation = generation;
        k.render = render;
        std::memcpy(k.world_to_view.data(), camera.world_to_view, sizeof camera.world_to_view);
        std::memcpy(k.view_to_clip.data(), camera.view_to_clip, sizeof camera.view_to_clip);
        k.toward_sun = toward_sun;
        k.ao_distance = settings.ao_distance;
        k.sun_size = settings.sun_size;
        k.casters = caster_classes(settings);
        return k;
    }

    bool trace_reset(const TraceKey &a, const TraceKey &b)
    {
        if (a.generation != b.generation || a.render != b.render || a.ao_distance != b.ao_distance || a.sun_size != b.sun_size || a.casters != b.casters)
            return true;
        if (matrix_moved(a.world_to_view, b.world_to_view) || matrix_moved(a.view_to_clip, b.view_to_clip))
            return true;
        for (int k = 0; k < 3; k++)
            if (!(std::fabs(a.toward_sun[k] - b.toward_sun[k]) <= 1e-5f))
                return true;
        return false;
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

    bool consistency_stale(uint64_t tested, uint64_t off)
    {
        return tested >= 32 && off * 20 > tested;
    }
}
