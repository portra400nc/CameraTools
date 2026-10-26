#include "compare.h"
#include "../enhancements.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>

namespace enhancements
{
    namespace
    {
        static_assert(ENHANCEMENTS_COMPARE_MAX == kCompareMax && sizeof(EnhancementsVariant) == 32 + sizeof(EnhancementsSettings));
        static_assert(sizeof(EnhancementsSettings) == 392, "settings_tsv writes every field: give a new one a column");
        // compare_remaining took the padding at the end, so a caller built before it still sends 72 bytes. The accumulator's
        // three fields follow, and the scene capture's three took the 4 bytes of padding after them and one more row, so a
        // caller of version 6 sends 88 and gets scene_face in its padding. The trace's five follow them, and the gather's four
        // follow those, the first in the 4 bytes of padding a caller of version 8 has. The restarts, their reason and the
        // shot follow, the first in the 4 bytes of padding a caller of version 9 has after its 132. The saved frames and the
        // depth of field's focus follow, then the lens flare's visibility.
        static_assert(offsetof(EnhancementsStatus, compare_remaining) == 68 && offsetof(EnhancementsStatus, accum_generation) == 80
            && offsetof(EnhancementsStatus, scene_face) == 84 && offsetof(EnhancementsStatus, trace_state) == 96 && offsetof(EnhancementsStatus, trace_target) == 116
            && offsetof(EnhancementsStatus, trace_restarts) == 132 && offsetof(EnhancementsStatus, shot_active) == 140
            && offsetof(EnhancementsStatus, frames_saved) == 144 && offsetof(EnhancementsStatus, dof_focus_meters) == 148
            && offsetof(EnhancementsStatus, flare_visibility) == 152 && sizeof(EnhancementsStatus) == 160);

        constexpr uint32_t kPeriod = kSettlePresents + 2;

        std::string format(const char *pattern, double value)
        {
            char text[32];
            std::snprintf(text, sizeof text, pattern, value);
            return text;
        }

        std::string number(float value) { return format("%g", value); }
    }

    std::optional<std::vector<Variant>> parse_variants(const EnhancementsVariant *raw, uint32_t count)
    {
        if (raw == nullptr || count == 0 || count > kCompareMax)
            return std::nullopt;
        std::vector<Variant> variants;
        for (uint32_t i = 0; i < count; i++)
        {
            const EnhancementsVariant &v = raw[i];
            if (v.settings.size < offsetof(EnhancementsSettings, enabled) + sizeof v.settings.enabled)
                return std::nullopt;
            std::string name(v.name, std::find(v.name, v.name + sizeof v.name, '\0'));
            if (name.empty())
                return std::nullopt;
            for (char &ch : name)
                if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '.' || ch == '_' || ch == '-'))
                    ch = '-';
            variants.push_back({ name, parse_settings(v.settings) });
        }
        return variants;
    }

    CompareTick compare_tick(uint32_t present, uint32_t count)
    {
        CompareTick tick;
        const uint32_t v = present / kPeriod, k = present % kPeriod;
        if (k == kPeriod - 1 && v < count)
            tick.copy = CompareTick::Copy { v, 0 };
        if (k == 0 && v >= 1 && v <= count)
            tick.copy = CompareTick::Copy { v - 1, 1 };
        if (k == kReadDelay && v >= 1 && v <= count)
            tick.read = v - 1;
        if (k == 0 && v < count)
            tick.apply = v;
        tick.restore = k == 0 && v == count;
        tick.done = present == count * kPeriod + kReadDelay;
        return tick;
    }

    uint32_t trace_wait_limit(uint32_t first, uint32_t passes, uint32_t pass_presents, uint32_t target)
    {
        if (passes <= first || passes >= target)
            return kTraceWaitPresents;
        const double pace = double(pass_presents) / (passes - first);
        const double limit = pass_presents + std::ceil(2.0 * pace * (target - passes));
        return static_cast<uint32_t>(std::clamp(limit, double(kTraceWaitPresents), double(kTraceWaitMost)));
    }

    bool compare_waits(uint32_t present, uint32_t count, uint32_t waited, bool short_of_samples, uint32_t limit)
    {
        const CompareTick tick = compare_tick(present, count);
        return tick.copy && tick.copy->frame == 0 && short_of_samples && waited < limit;
    }

    std::vector<uint8_t> to_rgb(const RawFrame &frame)
    {
        const size_t pixels = size_t(frame.size.width) * frame.size.height;
        const int red = frame.order == PixelOrder::rgba ? 0 : 2;
        std::vector<uint8_t> rgb(pixels * 3);
        for (size_t i = 0; i < pixels; i++)
        {
            const uint8_t *p = &frame.bytes[i * 4];
            rgb[i * 3] = p[red];
            rgb[i * 3 + 1] = p[1];
            rgb[i * 3 + 2] = p[2 - red];
        }
        return rgb;
    }

    Flicker flicker(const std::vector<uint8_t> &a, const std::vector<uint8_t> &b)
    {
        Flicker f;
        const size_t pixels = a.size() / 3;
        if (pixels == 0)
            return f;
        uint64_t changed = 0, total = 0;
        for (size_t i = 0; i < pixels * 3; i += 3)
        {
            uint32_t d = 0;
            for (size_t c = i; c < i + 3; c++)
                d = std::max<uint32_t>(d, static_cast<uint32_t>(std::abs(a[c] - b[c])));
            changed += d > 2;
            total += d;
            f.max = std::max(f.max, d);
        }
        f.changed = double(changed) / pixels;
        f.mean = double(total) / pixels;
        return f;
    }

    std::string png_name(uint32_t index, const std::string &name)
    {
        char prefix[16];
        std::snprintf(prefix, sizeof prefix, "%02u-", index);
        return prefix + name + ".png";
    }

    std::string compare_tsv(const std::vector<CompareRow> &rows)
    {
        const auto optional = [](const std::optional<uint32_t> &v) { return v ? std::to_string(*v) : std::string("-"); };
        std::string out = "index\tname\tstatus\tflicker\tflicker_max\tflicker_mean\twidth\theight\ttrace_samples\ttrace_target\ttrace_wait\ttrace_timeout\n";
        // A variant saved short of its samples says so in its status, as its picture is not what its settings give.
        const auto gave_up = [&optional](const CompareRow &r) {
            return r.trace_timeout ? "; gave up waiting for the trace at " + optional(r.trace_samples) + " of " + optional(r.trace_target) + " samples after "
                + std::to_string(r.trace_wait) + " presents" : std::string();
        };
        for (const CompareRow &r : rows)
            out += std::to_string(r.index) + '\t' + r.name + '\t' + r.line + gave_up(r) + '\t' + format("%.6f", r.flicker.changed) + '\t'
                + std::to_string(r.flicker.max) + '\t' + format("%.3f", r.flicker.mean) + '\t' + std::to_string(r.size.width) + '\t'
                + std::to_string(r.size.height) + '\t' + optional(r.trace_samples) + '\t' + optional(r.trace_target) + '\t'
                + std::to_string(r.trace_wait) + '\t' + (r.trace_timeout ? "1" : "0") + '\n';
        return out;
    }

    std::string settings_tsv(const std::vector<Variant> &variants)
    {
        std::string out = "index\tname\tenabled\tview\tflip\tambient.enabled\tambient.level\tambient.ao_strength\tambient.ao_radius"
                          "\tambient.foliage_ao_strength\tcontact_shadows.enabled\tcontact_shadows.length\tcontact_shadows.strength"
                          "\tcontact_shadows.thickness\tcontact_shadows.foliage_strength\tatmosphere.enabled\tatmosphere.density"
                          "\tatmosphere.height_falloff\tatmosphere.sun_scatter\tatmosphere.anisotropy\ttonemap.enabled\ttonemap.exposure_ev"
                          "\ttonemap.curve\ttonemap.bloom_strength\ttonemap.saturation\ttonemap.contrast\tsun_shadows.enabled"
                          "\tsun_shadows.light_size\tsun_shadows.min_penumbra\tsun_shadows.strength\twetness.enabled\twetness.wetness"
                          "\twetness.darkening\twetness.puddles\tlight_shafts.enabled\tlight_shafts.strength\tlight_shafts.density"
                          "\tlight_shafts.height\tlight_shafts.anisotropy\tlight_shafts.distance\tlight_shafts.patchiness"
                          "\tlight_shafts.patch_size\tlight_shafts.ambient\tlight_shafts.steps\ttrace.enabled\ttrace.ao\ttrace.sun"
                          "\ttrace.characters\ttrace.alpha_cards\ttrace.whole_cards\ttrace.ao_distance\ttrace.ao_strength\ttrace.sun_size\ttrace.sun_strength"
                          "\ttrace.ao_samples\ttrace.sun_samples\ttrace.budget_ms\tgather.enabled\tgather.samples\tgather.sky\tgather.filter"
                          "\tgather.strength\tgather.hit_ambient\tdepth_of_field.enabled\tdepth_of_field.f_number"
                          "\tdepth_of_field.sensor_height_mm\tdepth_of_field.focus_distance\tdepth_of_field.focus_x\tdepth_of_field.focus_y"
                          "\tdepth_of_field.focus_seconds\tdepth_of_field.max_blur\tdepth_of_field.blades\tdepth_of_field.blade_roundness"
                          "\tdepth_of_field.blade_rotation\tdepth_of_field.spherical_aberration\tdepth_of_field.cat_eye\tdepth_of_field.swirl"
                          "\tdepth_of_field.anamorphic\tdepth_of_field.chromatic\tdepth_of_field.samples\tdepth_of_field.highlight_threshold"
                          "\tdepth_of_field.highlight_gain\tdepth_of_field.scatter\tdepth_of_field.bokeh_contrast\tdepth_of_field.place"
                          "\tdepth_of_field.eye_distance\tdepth_of_field.eye_size\tlens_flare.enabled\tlens_flare.source\tlens_flare.x"
                          "\tlens_flare.y\tlens_flare.strength\tlens_flare.arcs\tlens_flare.ghosts\tlens_flare.streaks\tlens_flare.glare"
                          "\tlens_flare.size\tlens_flare.warmth\tlens_flare.chromatic\tlens_flare.visibility_seconds\tlens_flare.filled_arcs\tlens_flare.place\n";
        for (size_t i = 0; i < variants.size(); i++)
        {
            const Settings &s = variants[i].settings;
            const AmbientSettings &a = s.ambient;
            const ContactShadowSettings &c = s.contact_shadows;
            const AtmosphereSettings &h = s.atmosphere;
            const TonemapSettings &t = s.tonemap;
            const SunShadowSettings &u = s.sun_shadows;
            const WetnessSettings &w = s.wetness;
            const LightShaftSettings &f = s.light_shafts;
            const TraceSettings &r = s.trace;
            const GatherSettings &g = s.gather;
            const DepthOfFieldSettings &d = s.depth_of_field;
            const FlareSettings &l = s.flare;
            out += std::to_string(i) + '\t' + variants[i].name + '\t' + (s.enabled ? "1" : "0") + '\t' + kViews[static_cast<size_t>(s.view)].name
                + '\t' + (s.flip ? "1" : "0") + '\t' + (a.enabled ? "1" : "0") + '\t' + number(a.level) + '\t' + number(a.ao_strength) + '\t'
                + number(a.ao_radius) + '\t' + number(a.foliage_ao_strength) + '\t' + (c.enabled ? "1" : "0") + '\t' + number(c.length) + '\t'
                + number(c.strength) + '\t' + number(c.thickness) + '\t' + number(c.foliage_strength) + '\t' + (h.enabled ? "1" : "0") + '\t'
                + number(h.density) + '\t' + number(h.height_falloff) + '\t' + number(h.sun_scatter) + '\t' + number(h.anisotropy) + '\t'
                + (t.enabled ? "1" : "0") + '\t' + number(t.exposure_ev) + '\t' + kCurveNames[static_cast<size_t>(t.curve)] + '\t'
                + number(t.bloom_strength) + '\t' + number(t.saturation) + '\t' + number(t.contrast) + '\t' + (u.enabled ? "1" : "0") + '\t'
                + number(u.light_size) + '\t' + number(u.min_penumbra) + '\t' + number(u.strength) + '\t' + (w.enabled ? "1" : "0") + '\t'
                + number(w.wetness) + '\t' + number(w.darkening) + '\t' + number(w.puddles) + '\t' + (f.enabled ? "1" : "0") + '\t'
                + number(f.strength) + '\t' + number(f.density) + '\t' + number(f.height) + '\t' + number(f.anisotropy) + '\t'
                + number(f.distance) + '\t' + number(f.patchiness) + '\t' + number(f.patch_size) + '\t' + number(f.ambient) + '\t'
                + std::to_string(f.steps) + '\t' + (r.enabled ? "1" : "0") + '\t' + (r.ao ? "1" : "0") + '\t' + (r.sun ? "1" : "0") + '\t'
                + (r.characters ? "1" : "0") + '\t' + (r.alpha_cards ? "1" : "0") + '\t' + (r.whole_cards ? "1" : "0") + '\t' + number(r.ao_distance) + '\t' + number(r.ao_strength) + '\t'
                + number(r.sun_size) + '\t' + number(r.sun_strength) + '\t' + std::to_string(r.ao_samples) + '\t' + std::to_string(r.sun_samples) + '\t'
                + number(r.budget_ms) + '\t' + (g.enabled ? "1" : "0") + '\t' + std::to_string(g.samples) + '\t' + kGatherSkyNames[static_cast<size_t>(g.sky)]
                + '\t' + std::to_string(g.filter) + '\t' + number(g.strength) + '\t' + number(g.hit_ambient) + '\t' + (d.enabled ? "1" : "0") + '\t'
                + number(d.f_number) + '\t' + number(d.sensor_height_mm) + '\t' + number(d.focus_distance) + '\t' + number(d.focus_point[0]) + '\t'
                + number(d.focus_point[1]) + '\t' + number(d.focus_seconds) + '\t' + number(d.max_blur) + '\t' + std::to_string(d.blades) + '\t'
                + number(d.blade_roundness) + '\t' + number(d.blade_rotation) + '\t' + number(d.spherical_aberration) + '\t' + number(d.cat_eye) + '\t'
                + number(d.swirl) + '\t' + number(d.anamorphic) + '\t' + number(d.chromatic) + '\t' + std::to_string(d.samples) + '\t'
                + number(d.highlight_threshold) + '\t' + number(d.highlight_gain) + '\t' + (d.scatter ? "1" : "0") + '\t' + number(d.bokeh_contrast) + '\t'
                + (d.place == DofPlace::display ? "display" : "scene") + '\t' + number(d.eye_distance) + '\t' + number(d.eye_size) + '\t'
                + (l.enabled ? "1" : "0") + '\t' + (l.source == FlareSource::manual ? "manual" : "sun") + '\t' + number(l.position[0]) + '\t'
                + number(l.position[1]) + '\t' + number(l.strength) + '\t' + number(l.arcs) + '\t' + number(l.ghosts) + '\t' + number(l.streaks) + '\t'
                + number(l.glare) + '\t' + number(l.size) + '\t' + number(l.warmth) + '\t' + number(l.chromatic) + '\t' + number(l.visibility_seconds) + '\t' + number(l.filled_arcs) + '\t' + (l.place == FlarePlace::display ? "display" : "scene") + '\n';
        }
        return out;
    }
}
