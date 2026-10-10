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
        bool tonemap_wanted(const Settings &s) { return s.tonemap.enabled; }
        bool sun_shadows_wanted(const Settings &s) { return s.sun_shadows.enabled; }
        bool leaves_wanted(const Settings &s) { return s.leaves.enabled; }
        bool wetness_wanted(const Settings &s) { return s.wetness.enabled; }

        float finite_or(float value, float fallback, float low, float high)
        {
            return std::isfinite(value) ? std::fmin(std::fmax(value, low), high) : fallback;
        }

        constexpr uint32_t bit(PassId p) { return 1u << static_cast<uint32_t>(p); }

        static_assert(PHOTOREAL_PASS_AMBIENT == bit(PassId::ambient) && PHOTOREAL_PASS_CONTACT_SHADOWS == bit(PassId::contact_shadows)
            && PHOTOREAL_PASS_ATMOSPHERE == bit(PassId::atmosphere) && PHOTOREAL_PASS_TONEMAP == bit(PassId::tonemap)
            && PHOTOREAL_PASS_SUN_SHADOWS == bit(PassId::sun_shadows) && PHOTOREAL_PASS_LEAVES == bit(PassId::leaves)
            && PHOTOREAL_PASS_WETNESS == bit(PassId::wetness) && static_cast<int>(PassId::count) == 7);
        static_assert(PHOTOREAL_CURVE_AGX == static_cast<int>(Curve::agx) && PHOTOREAL_CURVE_COUNT == static_cast<int>(Curve::count));
        static_assert(PHOTOREAL_VIEW_COUNT == static_cast<int>(View::count) && PHOTOREAL_VIEW_STENCIL == static_cast<int>(View::stencil)
            && PHOTOREAL_VIEW_HDR_SCENE == static_cast<int>(View::hdr_scene) && PHOTOREAL_VIEW_BLOOM_FINAL == static_cast<int>(View::bloom_final)
            && PHOTOREAL_VIEW_SUN_ATLAS == static_cast<int>(View::sun_atlas));

        const char *const kErrorNames[] = { "none", "not-d3d11", "shader", "texture", "state" };
        static_assert(PHOTOREAL_ERROR_STATE == 4);
        // Callers built before foliage_ao_strength send 32 bytes, those before contact shadows 36, those before their
        // foliage_strength 52, those before atmosphere 56, those before tonemap 76, those before sun shadows 100, those
        // before leaves 116, those before wetness 128; csharp/Photoreal.cs marshals 144.
        static_assert(offsetof(PhotorealSettings, ambient) + offsetof(PhotorealAmbient, foliage_ao_strength) == 32
            && offsetof(PhotorealSettings, contact_shadows) == 36
            && offsetof(PhotorealSettings, contact_shadows) + offsetof(PhotorealContactShadows, foliage_strength) == 52
            && offsetof(PhotorealSettings, atmosphere) == 56 && offsetof(PhotorealSettings, tonemap) == 76
            && offsetof(PhotorealSettings, sun_shadows) == 100 && offsetof(PhotorealSettings, leaves) == 116 && offsetof(PhotorealSettings, wetness) == 128
            && sizeof(PhotorealSettings) == 144);
        static_assert(sizeof(PhotorealCamera) == 48 * sizeof(float));
        static_assert(PHOTOREAL_ACCUMULATE_PRESENT == static_cast<int>(AccumulateMode::present)
            && PHOTOREAL_ACCUMULATE_COUNT == static_cast<int>(AccumulateMode::count));
        static_assert(offsetof(PhotorealAccumulate, mode) == 4 && sizeof(PhotorealAccumulate) == 20);

    namespace hlsl
    {
        float saturate(float x) { return std::fmin(std::fmax(x, 0.0f), 1.0f); }
        float lerp(float a, float b, float t) { return a + (b - a) * t; }
        float smoothstep(float edge0, float edge1, float x)
        {
            const float t = saturate((x - edge0) / (edge1 - edge0));
            return t * t * (3 - 2 * t);
        }

#include "../shaders/wet.hlsli"
    }
    }

    const std::array<PassSpec, static_cast<size_t>(PassId::count)> kPasses = { {
        // The camera only for the puddles, which lie still in the world.
        { PassId::wetness, "wetness", Step::gbuffer_done,
          { Entry::normals, Entry::albedo, Entry::specular, Entry::material_id, Entry::smoothness, Entry::depth }, true, false, wetness_wanted },
        // Before contact shadows, which lower what it writes, and the camera only for its blur: it finds positions from
        // the game's own camera constants.
        { PassId::sun_shadows, "sun-shadows", Step::combine, { Entry::normals, Entry::depth, Entry::shadow_mask, Entry::sun_atlas }, true, true,
          sun_shadows_wanted },
        { PassId::contact_shadows, "contact-shadows", Step::combine, { Entry::normals, Entry::depth, Entry::shadow_mask }, true, false,
          contact_shadows_wanted },
        { PassId::ambient, "ambient", Step::combine, { Entry::normals, Entry::depth, Entry::ambient_diffuse }, true, false, ambient_wanted },
        { PassId::leaves, "leaves", Step::forward,
          { Entry::normals, Entry::albedo, Entry::material_id, Entry::depth, Entry::shadow_mask, Entry::hdr_scene }, true, false, leaves_wanted },
        { PassId::atmosphere, "atmosphere", Step::bloom, { Entry::depth, Entry::hdr_scene }, true, false, atmosphere_wanted },
        // bloom-final is optional: a tone map draw without one still gets ours, without bloom.
        { PassId::tonemap, "tonemap", Step::tonemap, { Entry::hdr_scene, Entry::tonemap_out }, false, false, tonemap_wanted },
    } };

    const std::array<const char *, static_cast<size_t>(Curve::count)> kCurveNames = { "game", "agx", "neutral" };

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
        // At the draw that samples it: the atlas then holds this frame's cascades.
        { View::sun_atlas, "sun-atlas", Entry::sun_atlas, Step::shadow_mask, Decode::gray },
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
        const TonemapSettings tonemap_defaults;
        const PhotorealTonemap &t = raw.tonemap;
        if (knows(offsetof(PhotorealSettings, tonemap), sizeof t))
        {
            s.tonemap.enabled = t.enabled != 0;
            s.tonemap.exposure_ev = finite_or(t.exposure_ev, tonemap_defaults.exposure_ev, -10.0f, 10.0f);
            s.tonemap.curve = t.curve < static_cast<uint32_t>(Curve::count) ? static_cast<Curve>(t.curve) : Curve::game;
            s.tonemap.bloom_strength = finite_or(t.bloom_strength, tonemap_defaults.bloom_strength, 0.0f, 4.0f);
            s.tonemap.saturation = finite_or(t.saturation, tonemap_defaults.saturation, 0.0f, 2.0f);
            s.tonemap.contrast = finite_or(t.contrast, tonemap_defaults.contrast, 0.5f, 2.0f);
        }
        const SunShadowSettings sun_defaults;
        const PhotorealSunShadows &u = raw.sun_shadows;
        if (knows(offsetof(PhotorealSettings, sun_shadows), sizeof u))
        {
            s.sun_shadows.enabled = u.enabled != 0;
            s.sun_shadows.light_size = finite_or(u.light_size, sun_defaults.light_size, 0.0f, 0.2f);
            s.sun_shadows.min_penumbra = finite_or(u.min_penumbra, sun_defaults.min_penumbra, 0.0f, 0.5f);
            s.sun_shadows.strength = finite_or(u.strength, sun_defaults.strength, 0.0f, 1.0f);
        }
        const LeavesSettings leaves_defaults;
        const PhotorealLeaves &l = raw.leaves;
        if (knows(offsetof(PhotorealSettings, leaves), sizeof l))
        {
            s.leaves.enabled = l.enabled != 0;
            s.leaves.strength = finite_or(l.strength, leaves_defaults.strength, 0.0f, 4.0f);
            s.leaves.scatter_sharpness = finite_or(l.scatter_sharpness, leaves_defaults.scatter_sharpness, 1.0f, 32.0f);
        }
        const WetnessSettings wetness_defaults;
        const PhotorealWetness &w = raw.wetness;
        if (knows(offsetof(PhotorealSettings, wetness), sizeof w))
        {
            s.wetness.enabled = w.enabled != 0;
            s.wetness.wetness = finite_or(w.wetness, wetness_defaults.wetness, 0.0f, 1.0f);
            s.wetness.darkening = finite_or(w.darkening, wetness_defaults.darkening, 0.0f, 1.0f);
            s.wetness.puddles = finite_or(w.puddles, wetness_defaults.puddles, 0.0f, 1.0f);
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
        c.wetness = finite_or(raw.sky_color[3], 0.0f, 0.0f, 1.0f);
        c.lens.x = finite_or(raw.lens_sample[0], 0.0f, -1.0f, 1.0f);
        c.lens.y = finite_or(raw.lens_sample[1], 0.0f, -1.0f, 1.0f);
        const float index = raw.lens_sample[2];
        c.lens.index = std::isfinite(index) && index >= 0 && index < 4294967296.0f ? static_cast<uint32_t>(index) : 0;
        c.lens.mark = std::isfinite(raw.lens_sample[3]) && raw.lens_sample[3] != 0;
        return c;
    }

    AccumulateSettings parse_accumulate(const PhotorealAccumulate &raw)
    {
        const auto knows = [&raw](size_t offset, size_t bytes) { return raw.size >= offset + bytes; };
        const AccumulateSettings defaults;
        AccumulateSettings a;
        if (knows(offsetof(PhotorealAccumulate, mode), sizeof raw.mode) && raw.mode < static_cast<uint32_t>(AccumulateMode::count))
            a.mode = static_cast<AccumulateMode>(raw.mode);
        if (knows(offsetof(PhotorealAccumulate, generation), sizeof raw.generation))
            a.generation = raw.generation;
        if (knows(offsetof(PhotorealAccumulate, cat_eye), sizeof raw.cat_eye))
            a.cat_eye = finite_or(raw.cat_eye, defaults.cat_eye, 0.0f, 1.0f);
        if (knows(offsetof(PhotorealAccumulate, cat_eye_falloff), sizeof raw.cat_eye_falloff))
            a.cat_eye_falloff = finite_or(raw.cat_eye_falloff, defaults.cat_eye_falloff, 0.01f, 1.0f);
        return a;
    }

    float wetness_amount(const WetnessSettings &settings, const Camera &camera)
    {
        return settings.wetness > 0 ? settings.wetness : camera.wetness;
    }

    float puddle_cover(float noise, float up, float wetness, float puddles) { return hlsl::puddle_cover(noise, up, wetness, puddles); }
    float wet_share(float wetness, float up, float puddle) { return hlsl::wet_share(wetness, up, puddle); }
    float wet_smoothness(float smoothness, float wet, float puddle) { return hlsl::wet_smoothness(smoothness, wet, puddle); }

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

    Plan plan(Step step, const FrameMap &map, const Settings &settings, const Inputs &inputs)
    {
        Plan p;
        const EntrySet found = map.found();
        for (const PassSpec &spec : kPasses)
        {
            if (spec.at != step)
                continue;
            Decision d { Decision::Kind::off, spec.id, {}, false, SunCheck::ok };
            if (settings.enabled && spec.wanted(settings))
            {
                d.missing = spec.needs.minus(found);
                d.camera_missing = spec.needs_camera && !inputs.camera;
                d.sun = spec.needs_sun ? inputs.sun : SunCheck::ok;
                d.kind = d.missing.empty() && !d.camera_missing && d.sun == SunCheck::ok ? Decision::Kind::run : Decision::Kind::skip;
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
                why[static_cast<size_t>(d.pass)] = { d.missing, false, d.camera_missing, false, d.sun };
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

    void FrameReport::finish(const FrameTracker &tracker, const Settings &settings, const Inputs &inputs)
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
            why[static_cast<size_t>(spec.id)] = { absent, absent.empty(), spec.needs_camera && !inputs.camera, false,
                spec.needs_sun ? inputs.sun : SunCheck::ok };
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
                if (why.sun != SunCheck::ok)
                    reasons += std::string(reasons.empty() ? "" : ",") + " sun constants " + kSunCheckNames[static_cast<size_t>(why.sun)];
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
        if (report.accumulate == AccumulateMode::add)
            line += "; accumulating";
        else if (report.accumulate == AccumulateMode::present)
            line += "; presenting " + std::to_string(report.accumulated) + " samples";
        if (report.compare_count != 0)
            line += "; comparing " + std::to_string(report.compare_saved) + "/" + std::to_string(report.compare_count);
        if (report.error != PHOTOREAL_ERROR_NONE && report.error < std::size(kErrorNames))
            line += std::string("; error ") + kErrorNames[report.error];
        return line;
    }
}
