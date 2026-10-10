#include "plan.h"
#include "../photoreal.h"

#include <cmath>
#include <cstddef>
#include <cstring>
#include <iterator>

namespace photoreal
{
    namespace
    {
        bool ambient_wanted(const Settings &s) { return s.ambient.enabled; }
        bool contact_shadows_wanted(const Settings &s) { return s.contact_shadows.enabled; }
        bool atmosphere_wanted(const Settings &s) { return s.atmosphere.enabled; }

        float finite_or(float value, float fallback, float low, float high)
        {
            return std::isfinite(value) ? std::fmin(std::fmax(value, low), high) : fallback;
        }

        constexpr uint32_t bit(PassId p) { return 1u << static_cast<uint32_t>(p); }

        static_assert(PHOTOREAL_PASS_AMBIENT == bit(PassId::ambient) && PHOTOREAL_PASS_CONTACT_SHADOWS == bit(PassId::contact_shadows)
            && PHOTOREAL_PASS_ATMOSPHERE == bit(PassId::atmosphere) && static_cast<int>(PassId::count) == 3);
        static_assert(PHOTOREAL_VIEW_COUNT == static_cast<int>(View::count) && PHOTOREAL_VIEW_STENCIL == static_cast<int>(View::stencil)
            && PHOTOREAL_VIEW_HDR_SCENE == static_cast<int>(View::hdr_scene) && PHOTOREAL_VIEW_BLOOM_FINAL == static_cast<int>(View::bloom_final));

        const char *const kErrorNames[] = { "none", "not-d3d11", "shader", "texture", "state" };
        static_assert(PHOTOREAL_ERROR_STATE == 4);
        // Callers built before foliage_ao_strength send 32 bytes, those before contact shadows 36, those before their
        // foliage_strength 52, those before atmosphere 56; csharp/Photoreal.cs marshals 76.
        static_assert(offsetof(PhotorealSettings, ambient) + offsetof(PhotorealAmbient, foliage_ao_strength) == 32
            && offsetof(PhotorealSettings, contact_shadows) == 36
            && offsetof(PhotorealSettings, contact_shadows) + offsetof(PhotorealContactShadows, foliage_strength) == 52
            && offsetof(PhotorealSettings, atmosphere) == 56 && sizeof(PhotorealSettings) == 76);
        static_assert(sizeof(PhotorealCamera) == 44 * sizeof(float));
    }

    const std::array<PassSpec, static_cast<size_t>(PassId::count)> kPasses = { {
        { PassId::contact_shadows, "contact-shadows", Step::combine, { Entry::normals, Entry::depth, Entry::shadow_mask }, true, contact_shadows_wanted },
        { PassId::ambient, "ambient", Step::combine, { Entry::normals, Entry::depth, Entry::ambient_diffuse }, true, ambient_wanted },
        { PassId::atmosphere, "atmosphere", Step::bloom, { Entry::depth, Entry::hdr_scene }, true, atmosphere_wanted },
    } };

    const std::array<ViewSpec, static_cast<size_t>(View::count)> kViews = { {
        { View::off, "off", Entry::count, Step::count, Decode::rgb },
        { View::normals, "normals", Entry::normals, Step::combine, Decode::rgb },
        { View::albedo, "albedo", Entry::albedo, Step::combine, Decode::rgb },
        { View::specular, "specular", Entry::specular, Step::combine, Decode::rgb },
        { View::material_id, "material-id", Entry::material_id, Step::combine, Decode::material_palette },
        { View::smoothness, "smoothness", Entry::smoothness, Step::combine, Decode::gray },
        { View::character, "character", Entry::character, Step::combine, Decode::gray },
        { View::depth, "depth", Entry::depth, Step::combine, Decode::depth_reversed },
        { View::stencil, "stencil", Entry::depth, Step::combine, Decode::stencil_palette },
        { View::shadow_mask, "shadow-mask", Entry::shadow_mask, Step::combine, Decode::gray },
        { View::quarter_shadow, "quarter-shadow", Entry::quarter_shadow, Step::combine, Decode::gray },
        { View::ambient_diffuse, "ambient-diffuse", Entry::ambient_diffuse, Step::combine, Decode::hdr },
        { View::ambient_specular, "ambient-specular", Entry::ambient_specular, Step::combine, Decode::hdr },
        { View::hdr_scene, "hdr-scene", Entry::hdr_scene, Step::tonemap, Decode::hdr },
        { View::bloom_final, "bloom-final", Entry::bloom_final, Step::tonemap, Decode::hdr },
    } };

    Label encode_label(std::string_view text)
    {
        Label label;
        for (const char ch : text.substr(0, label.glyphs.size()))
        {
            uint8_t glyph = 38;
            if (ch == ' ')
                glyph = 0;
            else if (ch >= 'a' && ch <= 'z')
                glyph = static_cast<uint8_t>(1 + ch - 'a');
            else if (ch >= 'A' && ch <= 'Z')
                glyph = static_cast<uint8_t>(1 + ch - 'A');
            else if (ch >= '0' && ch <= '9')
                glyph = static_cast<uint8_t>(27 + ch - '0');
            else if (ch == '-')
                glyph = 37;
            label.glyphs[label.length++] = glyph;
        }
        return label;
    }

    Label view_label(View view, bool found)
    {
        const std::string name = kViews[static_cast<size_t>(view)].name;
        return encode_label(found ? name : name + " missing");
    }

    Settings parse_settings(const PhotorealSettings &raw)
    {
        const auto knows = [&raw](size_t offset, size_t bytes) { return raw.size >= offset + bytes; };
        Settings s;
        if (knows(offsetof(PhotorealSettings, enabled), sizeof raw.enabled))
            s.enabled = raw.enabled != 0;
        if (knows(offsetof(PhotorealSettings, view), sizeof raw.view) && raw.view < static_cast<uint32_t>(View::count))
            s.view = static_cast<View>(raw.view);
        if (knows(offsetof(PhotorealSettings, flip), sizeof raw.flip))
            s.flip = raw.flip != 0;
        const AmbientSettings defaults;
        constexpr size_t ambient = offsetof(PhotorealSettings, ambient);
        if (knows(ambient, offsetof(PhotorealAmbient, foliage_ao_strength)))  // the block as version 1 first shipped it
        {
            s.ambient.enabled = raw.ambient.enabled != 0;
            s.ambient.level = finite_or(raw.ambient.level, defaults.level, 0.0f, 4.0f);
            s.ambient.ao_strength = finite_or(raw.ambient.ao_strength, defaults.ao_strength, 0.0f, 1.0f);
            s.ambient.ao_radius = finite_or(raw.ambient.ao_radius, defaults.ao_radius, 0.05f, 10.0f);
        }
        if (knows(ambient + offsetof(PhotorealAmbient, foliage_ao_strength), sizeof raw.ambient.foliage_ao_strength))
            s.ambient.foliage_ao_strength = finite_or(raw.ambient.foliage_ao_strength, defaults.foliage_ao_strength, 0.0f, 1.0f);
        const ContactShadowSettings contact_defaults;
        const PhotorealContactShadows &c = raw.contact_shadows;
        constexpr size_t contact = offsetof(PhotorealSettings, contact_shadows);
        if (knows(contact, offsetof(PhotorealContactShadows, foliage_strength)))  // the block as version 2 first shipped it
        {
            s.contact_shadows.enabled = c.enabled != 0;
            s.contact_shadows.length = finite_or(c.length, contact_defaults.length, 0.05f, 5.0f);
            s.contact_shadows.strength = finite_or(c.strength, contact_defaults.strength, 0.0f, 1.0f);
            s.contact_shadows.thickness = finite_or(c.thickness, contact_defaults.thickness, 0.01f, 2.0f);
        }
        if (knows(contact + offsetof(PhotorealContactShadows, foliage_strength), sizeof c.foliage_strength))
            s.contact_shadows.foliage_strength = finite_or(c.foliage_strength, contact_defaults.foliage_strength, 0.0f, 1.0f);
        const AtmosphereSettings atmosphere_defaults;
        const PhotorealAtmosphere &a = raw.atmosphere;
        if (knows(offsetof(PhotorealSettings, atmosphere), sizeof a))
        {
            s.atmosphere.enabled = a.enabled != 0;
            s.atmosphere.density = finite_or(a.density, atmosphere_defaults.density, 0.0f, 0.01f);
            s.atmosphere.height_falloff = finite_or(a.height_falloff, atmosphere_defaults.height_falloff, 0.0f, 1.0f);
            s.atmosphere.sun_scatter = finite_or(a.sun_scatter, atmosphere_defaults.sun_scatter, 0.0f, 10.0f);
            s.atmosphere.anisotropy = finite_or(a.anisotropy, atmosphere_defaults.anisotropy, -0.95f, 0.95f);
        }
        return s;
    }

    Camera parse_camera(const PhotorealCamera &raw)
    {
        Camera c {};
        std::memcpy(c.world_to_view, raw.world_to_view, sizeof c.world_to_view);
        std::memcpy(c.view_to_clip, raw.view_to_clip, sizeof c.view_to_clip);
        const double x = raw.sun_direction[0], y = raw.sun_direction[1], z = raw.sun_direction[2];
        const double length = std::sqrt(x * x + y * y + z * z);
        if (std::isfinite(length) && length > 0)
            for (int i = 0; i < 3; i++)
                c.toward_sun[i] = static_cast<float>(raw.sun_direction[i] / length);
        for (int i = 0; i < 3; i++)
        {
            c.sun_color[i] = finite_or(raw.sun_color[i], 0.0f, 0.0f, INFINITY);
            c.sky_color[i] = finite_or(raw.sky_color[i], 0.0f, 0.0f, INFINITY);
        }
        return c;
    }

    bool invert(const float m[16], float out[16])
    {
        float inv[16];
        inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
        inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
        inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
        inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
        inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
        inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
        inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
        inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
        inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
        inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
        inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
        inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
        inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
        inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
        inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
        inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
        const float det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
        if (det == 0 || !std::isfinite(det))
            return false;
        for (int i = 0; i < 16; i++)
            out[i] = inv[i] / det;
        return true;
    }

    Plan plan(Step step, const FrameMap &map, const Settings &settings, bool camera_known)
    {
        Plan p;
        const EntrySet found = map.found();
        for (const PassSpec &spec : kPasses)
        {
            if (spec.at != step)
                continue;
            Decision d { Decision::Kind::off, spec.id, {}, false };
            if (settings.enabled && spec.wanted(settings))
            {
                d.missing = spec.needs.minus(found);
                d.camera_missing = spec.needs_camera && !camera_known;
                d.kind = d.missing.empty() && !d.camera_missing ? Decision::Kind::run : Decision::Kind::skip;
            }
            p.items[p.count++] = d;
        }
        return p;
    }

    StepSet wanted_steps(const Settings &settings)
    {
        StepSet steps;
        if (!settings.enabled)
            return steps;
        for (const PassSpec &spec : kPasses)
            if (spec.wanted(settings))
                steps.add(spec.at);
        if (settings.view != View::off)
            steps.add(kViews[static_cast<size_t>(settings.view)].snapshot_at);
        for (StepSet closed; closed.bits != steps.bits;)
        {
            closed = steps;
            for (const StepSpec &spec : kRecipe)
                if (closed.has(spec.step))
                    steps |= spec.after;
        }
        return steps;
    }

    FrameReport FrameReport::start(const Settings &settings)
    {
        FrameReport report;
        report.armed = settings.enabled;
        report.view = settings.enabled ? settings.view : View::off;
        return report;
    }

    void FrameReport::note(const Plan &plan)
    {
        for (uint8_t i = 0; i < plan.count; i++)
        {
            const Decision &d = plan.items[i];
            if (d.kind == Decision::Kind::run)
                ran.add(d.pass);
            else if (d.kind == Decision::Kind::skip)
            {
                skipped.add(d.pass);
                why[static_cast<size_t>(d.pass)] = { d.missing, false, d.camera_missing, false };
                missing |= d.missing;
            }
        }
    }

    bool FrameReport::note_view(Step step, const FrameMap &map)
    {
        if (view == View::off)
            return false;
        const ViewSpec &spec = kViews[static_cast<size_t>(view)];
        if (spec.snapshot_at != step)
            return false;
        view_found = map[spec.entry].has_value();
        if (!view_found)
            missing.add(spec.entry);
        return view_found;
    }

    void FrameReport::note_failed(PassId pass)
    {
        ran.remove(pass);
        skipped.add(pass);
        why[static_cast<size_t>(pass)] = { {}, false, false, true };
    }

    void FrameReport::finish(const FrameTracker &tracker, const Settings &settings, bool camera_known)
    {
        render = tracker.map().render;
        found = tracker.matched();
        restarts = tracker.restarts();
        wanted = wanted_steps(settings);
        const EntrySet entries = tracker.map().found();
        for (const PassSpec &spec : kPasses)
        {
            if (!settings.enabled || !spec.wanted(settings) || found.has(spec.at))
                continue;
            const EntrySet absent = spec.needs.minus(entries);
            skipped.add(spec.id);
            why[static_cast<size_t>(spec.id)] = { absent, absent.empty(), spec.needs_camera && !camera_known, false };
            missing |= absent;
        }
        if (view != View::off && !entries.has(kViews[static_cast<size_t>(view)].entry))
            missing.add(kViews[static_cast<size_t>(view)].entry);
    }

    std::string describe(const FrameReport &report)
    {
        std::string line;
        if (!report.armed)
            line = "off";
        else
        {
            if (report.found.empty())
                line = "found nothing";
            else
            {
                line = std::to_string(report.render.width) + "x" + std::to_string(report.render.height) + " found";
                for (const StepSpec &spec : kRecipe)
                    if (report.found.has(spec.step))
                        line += std::string(" ") + spec.name;
            }
            if (!report.ran.empty())
            {
                line += "; ran";
                for (const PassSpec &spec : kPasses)
                    if (report.ran.has(spec.id))
                        line += std::string(" ") + spec.name + "@" + kRecipe[static_cast<size_t>(spec.at)].name;
            }
            for (const PassSpec &spec : kPasses)
            {
                if (!report.skipped.has(spec.id))
                    continue;
                const SkipReason &why = report.why[static_cast<size_t>(spec.id)];
                line += std::string("; skipped ") + spec.name + ":";
                std::string reasons;
                if (!why.missing.empty())
                {
                    reasons += " missing";
                    for (size_t e = 0; e < kEntryNames.size(); e++)
                        if (why.missing.has(static_cast<Entry>(e)))
                            reasons += std::string(" ") + kEntryNames[e];
                }
                if (why.step_missed)
                    reasons += std::string(reasons.empty() ? "" : ",") + " no " + kRecipe[static_cast<size_t>(spec.at)].name;
                if (why.camera_missing)
                    reasons += std::string(reasons.empty() ? "" : ",") + " no camera";
                if (why.failed)
                    reasons += std::string(reasons.empty() ? "" : ",") + " no texture";
                line += reasons;
            }
            if (report.view != View::off)
            {
                const ViewSpec &spec = kViews[static_cast<size_t>(report.view)];
                line += std::string("; view ") + spec.name;
                if (!report.found.has(spec.snapshot_at))
                    line += std::string(": no ") + kRecipe[static_cast<size_t>(spec.snapshot_at)].name;
                else if (!report.view_found)
                    line += std::string(": missing ") + kEntryNames[static_cast<size_t>(spec.entry)];
            }
            if (report.restarts != 0)
                line += "; restarts " + std::to_string(report.restarts);
        }
        if (report.compare_count != 0)
            line += "; comparing " + std::to_string(report.compare_saved) + "/" + std::to_string(report.compare_count);
        if (report.error != PHOTOREAL_ERROR_NONE && report.error < std::size(kErrorNames))
            line += std::string("; error ") + kErrorNames[report.error];
        return line;
    }
}
