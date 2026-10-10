// The comparison capture's decisions: parsing variants, the present schedule, pixels, flicker and the TSV files.
#include "../core/compare.h"
#include "../photoreal.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace photoreal;

namespace
{
    int failures = 0;

    void check(const char *name, bool ok)
    {
        std::printf("%s %s\n", ok ? "PASS" : "FAIL", name);
        failures += ok ? 0 : 1;
    }

    PhotorealVariant variant(const char *name, uint32_t enabled)
    {
        PhotorealVariant v {};
        std::strncpy(v.name, name, sizeof v.name);
        v.settings.size = sizeof v.settings;
        v.settings.enabled = enabled;
        v.settings.flip = 1;
        v.settings.ambient = { 1, 0.6f, 0.5f, 1, 0.5f };
        v.settings.contact_shadows = { 0, 0.6f, 1, 0.25f, 0 };
        return v;
    }

    std::string text(const CompareTick &t)
    {
        std::string out;
        if (t.copy)
            out += " copy " + std::to_string(t.copy->variant) + "." + std::to_string(t.copy->frame);
        if (t.read)
            out += " read " + std::to_string(*t.read);
        if (t.apply)
            out += " apply " + std::to_string(*t.apply);
        if (t.restore)
            out += " restore";
        if (t.done)
            out += " done";
        return out.empty() ? "-" : out.substr(1);
    }
}

int main()
{
    {
        std::vector<std::string> ticks;
        for (uint32_t present = 0; present < 16; present++)
            ticks.push_back(text(compare_tick(present, 2)));
        check("two variants: apply, settle 4, copy 2, apply the next at the second copy, read 2 later, restore, done",
            ticks == std::vector<std::string> { "apply 0", "-", "-", "-", "-", "copy 0.0", "copy 0.1 apply 1", "-", "read 0", "-", "-",
                "copy 1.0", "copy 1.1 restore", "-", "read 1 done", "-" });
        check("sixteen variants end at present 98", text(compare_tick(96, 16)) == "copy 15.1 restore" && text(compare_tick(98, 16)) == "read 15 done");
    }
    {
        PhotorealVariant raw[] = { variant("off", 0), variant("ambient on/2", 1), variant("", 1) };
        std::memset(raw[2].name, 'x', sizeof raw[2].name);
        raw[1].settings.ambient.level = 9;
        const auto parsed = parse_variants(raw, 3);
        check("three variants parse", parsed && parsed->size() == 3);
        check("names keep letters, digits and '-', other bytes become '-', and a full 32-byte name has no NUL",
            parsed && (*parsed)[0].name == "off" && (*parsed)[1].name == "ambient-on-2" && (*parsed)[2].name == std::string(32, 'x'));
        check("each variant's settings parse like PhotorealApply's: off, then ambient level 9 clamped to 4",
            parsed && !(*parsed)[0].settings.enabled && (*parsed)[1].settings.enabled && (*parsed)[1].settings.ambient.level == 4.0f);
        PhotorealVariant utf8 = variant("\xc3\xbc" "ber", 1);
        check("each non-ASCII UTF-8 byte becomes '-'", parse_variants(&utf8, 1) && (*parse_variants(&utf8, 1))[0].name == "--ber");
    }
    {
        PhotorealVariant raw[17];
        for (PhotorealVariant &v : raw)
            v = variant("a", 1);
        check("null, zero and seventeen variants are invalid, sixteen are not", !parse_variants(nullptr, 1) && !parse_variants(raw, 0)
            && !parse_variants(raw, 17) && parse_variants(raw, 16));
        PhotorealVariant empty = variant("", 1);
        check("an empty name is invalid", !parse_variants(&empty, 1));
        PhotorealVariant short_settings = variant("a", 1);
        short_settings.settings.size = 4;
        check("settings too short to hold enabled are invalid", !parse_variants(&short_settings, 1));
    }
    {
        const RawFrame bgra { { 2, 1 }, PixelOrder::bgra, { 10, 20, 30, 255, 1, 2, 3, 0 } };
        check("BGRA reads as RGB with alpha dropped", to_rgb(bgra) == std::vector<uint8_t> { 30, 20, 10, 3, 2, 1 });
        const RawFrame rgba { { 2, 1 }, PixelOrder::rgba, { 10, 20, 30, 255, 1, 2, 3, 0 } };
        check("RGBA reads as RGB with alpha dropped", to_rgb(rgba) == std::vector<uint8_t> { 10, 20, 30, 1, 2, 3 });
    }
    {
        const std::vector<uint8_t> a = { 0, 0, 0, 100, 100, 100, 50, 50, 50, 7, 7, 7 };
        const std::vector<uint8_t> b = { 0, 0, 0, 102, 99, 100, 50, 60, 45, 0, 7, 7 };
        const Flicker f = flicker(a, b);
        check("flicker: of differences 0, 2, 10 and 7, two are over 2, the max is 10 and the mean 4.75",
            f.changed == 0.5 && f.max == 10 && f.mean == 4.75);
        check("identical frames do not flicker", flicker(a, a).changed == 0 && flicker(a, a).max == 0 && flicker(a, a).mean == 0);
    }
    {
        check("PNG names are the two-digit index and the name", png_name(3, "ambient") == "03-ambient.png" && png_name(15, "x") == "15-x.png");
        const std::vector<CompareRow> rows = { { 0, "off", "off; comparing 0/2", { 0, 0, 0 }, { 1280, 800 } },
            { 1, "ambient", "1152x720 found gbuffer; ran ambient@combine", { 0.0123456, 40, 0.5 }, { 1280, 800 } } };
        check("compare.tsv has a header and one row per variant", compare_tsv(rows)
            == "index\tname\tstatus\tflicker\tflicker_max\tflicker_mean\twidth\theight\n"
               "0\toff\toff; comparing 0/2\t0.000000\t0\t0.000\t1280\t800\n"
               "1\tambient\t1152x720 found gbuffer; ran ambient@combine\t0.012346\t40\t0.500\t1280\t800\n");
    }
    {
        PhotorealVariant raw[] = { variant("off", 0), variant("shadows", 1) };
        raw[1].settings.view = PHOTOREAL_VIEW_SHADOW_MASK;
        raw[1].settings.contact_shadows = { 1, 1.2f, 0.7f, 0.3f, 0.4f };
        check("settings.tsv writes every parsed field, one row per variant", settings_tsv(*parse_variants(raw, 2))
            == "index\tname\tenabled\tview\tflip\tambient.enabled\tambient.level\tambient.ao_strength\tambient.ao_radius"
               "\tambient.foliage_ao_strength\tcontact_shadows.enabled\tcontact_shadows.length\tcontact_shadows.strength"
               "\tcontact_shadows.thickness\tcontact_shadows.foliage_strength\n"
               "0\toff\t0\toff\t1\t1\t0.6\t0.5\t1\t0.5\t0\t0.6\t1\t0.25\t0\n"
               "1\tshadows\t1\tshadow-mask\t1\t1\t0.6\t0.5\t1\t0.5\t1\t1.2\t0.7\t0.3\t0.4\n");
    }
    std::printf("%s: %d failed\n", __FILE__, failures);
    return failures == 0 ? 0 : 1;
}
