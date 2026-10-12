#include "compare.h"
#include "../photoreal.h"

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstdlib>

namespace photoreal
{
    namespace
    {
        static_assert(PHOTOREAL_COMPARE_MAX == kCompareMax && sizeof(PhotorealVariant) == 32 + sizeof(PhotorealSettings));
        static_assert(sizeof(PhotorealSettings) == 212, "settings_tsv writes every field: give a new one a column");
        // compare_remaining took the padding at the end, so a caller built before it still sends 72 bytes. The accumulator's
        // three fields follow, and the scene capture's three took the 4 bytes of padding after them and one more row, so a
        // caller of version 6 sends 88 and gets scene_face in its padding. The trace's five follow them.
        static_assert(offsetof(PhotorealStatus, compare_remaining) == 68 && offsetof(PhotorealStatus, accum_generation) == 80
            && offsetof(PhotorealStatus, scene_face) == 84 && offsetof(PhotorealStatus, trace_state) == 96 && sizeof(PhotorealStatus) == 120);

        constexpr uint32_t kPeriod = kSettlePresents + 2;

        std::string format(const char *pattern, double value)
        {
            char text[32];
            std::snprintf(text, sizeof text, pattern, value);
            return text;
        }

        std::string number(float value) { return format("%g", value); }
    }

    std::optional<std::vector<Variant>> parse_variants(const PhotorealVariant *raw, uint32_t count)
    {
        if (raw == nullptr || count == 0 || count > kCompareMax)
            return std::nullopt;
        std::vector<Variant> variants;
        for (uint32_t i = 0; i < count; i++)
        {
            const PhotorealVariant &v = raw[i];
            if (v.settings.size < offsetof(PhotorealSettings, enabled) + sizeof v.settings.enabled)
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

    bool compare_waits(uint32_t present, uint32_t count, uint32_t waited, bool short_of_samples)
    {
        const CompareTick tick = compare_tick(present, count);
        return tick.copy && tick.copy->frame == 0 && short_of_samples && waited < kTraceWaitPresents;
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
        for (const CompareRow &r : rows)
            out += std::to_string(r.index) + '\t' + r.name + '\t' + r.line + '\t' + format("%.6f", r.flicker.changed) + '\t'
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
                          "\ttrace.characters\ttrace.alpha_cards\ttrace.ao_distance\ttrace.ao_strength\ttrace.sun_size\ttrace.sun_strength"
                          "\ttrace.ao_samples\ttrace.sun_samples\ttrace.budget_ms\n";
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
                + (r.characters ? "1" : "0") + '\t' + (r.alpha_cards ? "1" : "0") + '\t' + number(r.ao_distance) + '\t' + number(r.ao_strength) + '\t'
                + number(r.sun_size) + '\t' + number(r.sun_strength) + '\t' + std::to_string(r.ao_samples) + '\t' + std::to_string(r.sun_samples) + '\t'
                + number(r.budget_ms) + '\n';
        }
        return out;
    }
}
