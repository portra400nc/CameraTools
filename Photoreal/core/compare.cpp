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
        static_assert(sizeof(PhotorealSettings) == 56, "settings_tsv writes every field: give a new one a column");
        // compare_remaining took the padding at the end, so a caller built before it still sends 72 bytes.
        static_assert(offsetof(PhotorealStatus, compare_remaining) == 68 && sizeof(PhotorealStatus) == 72);

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
        std::string out = "index\tname\tstatus\tflicker\tflicker_max\tflicker_mean\twidth\theight\n";
        for (const CompareRow &r : rows)
            out += std::to_string(r.index) + '\t' + r.name + '\t' + r.line + '\t' + format("%.6f", r.flicker.changed) + '\t'
                + std::to_string(r.flicker.max) + '\t' + format("%.3f", r.flicker.mean) + '\t' + std::to_string(r.size.width) + '\t'
                + std::to_string(r.size.height) + '\n';
        return out;
    }

    std::string settings_tsv(const std::vector<Variant> &variants)
    {
        std::string out = "index\tname\tenabled\tview\tflip\tambient.enabled\tambient.level\tambient.ao_strength\tambient.ao_radius"
                          "\tambient.foliage_ao_strength\tcontact_shadows.enabled\tcontact_shadows.length\tcontact_shadows.strength"
                          "\tcontact_shadows.thickness\tcontact_shadows.foliage_strength\n";
        for (size_t i = 0; i < variants.size(); i++)
        {
            const Settings &s = variants[i].settings;
            const AmbientSettings &a = s.ambient;
            const ContactShadowSettings &c = s.contact_shadows;
            out += std::to_string(i) + '\t' + variants[i].name + '\t' + (s.enabled ? "1" : "0") + '\t' + kViews[static_cast<size_t>(s.view)].name
                + '\t' + (s.flip ? "1" : "0") + '\t' + (a.enabled ? "1" : "0") + '\t' + number(a.level) + '\t' + number(a.ao_strength) + '\t'
                + number(a.ao_radius) + '\t' + number(a.foliage_ao_strength) + '\t' + (c.enabled ? "1" : "0") + '\t' + number(c.length) + '\t'
                + number(c.strength) + '\t' + number(c.thickness) + '\t' + number(c.foliage_strength) + '\n';
        }
        return out;
    }
}
