// The comparison capture's decisions: parsing variants, the present schedule, pixels, flicker and the TSV files.
#include "../core/compare.h"
#include "../enhancements.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace enhancements;

namespace
{
    int failures = 0;

    void check(const char *name, bool ok)
    {
        std::printf("%s %s\n", ok ? "PASS" : "FAIL", name);
        failures += ok ? 0 : 1;
    }

    EnhancementsVariant variant(const char *name, uint32_t enabled)
    {
        EnhancementsVariant v {};
        std::strncpy(v.name, name, sizeof v.name);
        v.settings.size = sizeof v.settings;
        v.settings.enabled = enabled;
        v.settings.flip = 1;
        v.settings.ambient = { 1, 0.6f, 0.5f, 1, 0.5f };
        v.settings.contact_shadows = { 0, 0.6f, 1, 0.25f, 0 };
        v.settings.atmosphere = { 0, 0.000325f, 0.02f, 1, 0.7f };
        v.settings.tonemap = { 0, 0, ENHANCEMENTS_CURVE_GAME, 1, 1, 1 };
        v.settings.sun_shadows = { 0, 0.03f, 0.02f, 1 };
        v.settings.wetness = { 0, 0, 0.35f, 0.3f };
        v.settings.trace = { 0, 0, ENHANCEMENTS_TRACE_CAST_CHARACTERS | ENHANCEMENTS_TRACE_CAST_ALPHA_CARDS, 2, 1, 0.0093f, 1, 64, 32, 20 };
        v.settings.gather = { 0, 128, ENHANCEMENTS_GATHER_SKY_VISIBILITY, 2, 1, 0.5f };
        v.settings.light_shafts = { 0, 1, 0.01f, 10, 0.6f, 150, 0.5f, 6, 1, 48 };
        v.settings.depth_of_field = { 0, 2.8f, 24, 0, { 0.5f, 0.5f }, 0.9f, 0.03f, 0, 0.5f, 0, 0.6f, 0, 0, 1, 0, 49, 2, 2, 1, 1, 0, 0, 0.015f };
        v.settings.lens_flare = { 0, ENHANCEMENTS_FLARE_SOURCE_SUN, { 0.8f, 0.12f }, 1, 1, 1, 1, 1, 1, 0.5f, 1, 0.15f, 1, ENHANCEMENTS_FLARE_PLACE_SCENE };
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
        std::vector<uint32_t> waits;
        for (uint32_t present = 0; present < 16; present++)
            if (compare_waits(present, 2, 0, true))
                waits.push_back(present);
        check("a variant short of trace samples waits only at its first copy, presents 5 and 11 of two variants", waits == std::vector<uint32_t> { 5, 11 });
        check("and not once it has its samples or has waited 1800 presents", !compare_waits(5, 2, 0, false) && compare_waits(5, 2, 1799, true)
            && !compare_waits(5, 2, 1800, true));
        check("the wait's limit follows the trace's pace: 1800 presents before a pass comes in; build 209's 94 of 128 passes in 1800 presents get "
              "3103, where the old limit cut them off; 30 of 64 passes in 300 presents keep 1800; a pace of 500 presents a pass stops at 36000; and "
              "64 passes on top of 64 at 20 presents each, 10 of them in, get 200 + 2160",
            trace_wait_limit(0, 0, 0, 128) == 1800 && trace_wait_limit(0, 94, 1800, 128) == 3103 && compare_waits(5, 2, 1800, true, 3103)
            && !compare_waits(5, 2, 3103, true, 3103) && trace_wait_limit(0, 30, 300, 64) == 1800 && trace_wait_limit(0, 4, 2000, 128) == kTraceWaitMost
            && trace_wait_limit(64, 64, 0, 128) == 1800 && trace_wait_limit(64, 74, 200, 128) == 2360 && trace_wait_limit(0, 128, 2451, 128) == 1800);
    }
    {
        EnhancementsVariant raw[] = { variant("off", 0), variant("ambient on/2", 1), variant("", 1) };
        std::memset(raw[2].name, 'x', sizeof raw[2].name);
        raw[1].settings.ambient.level = 9;
        const auto parsed = parse_variants(raw, 3);
        check("three variants parse", parsed && parsed->size() == 3);
        check("names keep letters, digits and '-', other bytes become '-', and a full 32-byte name has no NUL",
            parsed && (*parsed)[0].name == "off" && (*parsed)[1].name == "ambient-on-2" && (*parsed)[2].name == std::string(32, 'x'));
        check("each variant's settings parse like EnhancementsApply's: off, then ambient level 9 clamped to 4",
            parsed && !(*parsed)[0].settings.enabled && (*parsed)[1].settings.enabled && (*parsed)[1].settings.ambient.level == 4.0f);
        EnhancementsVariant utf8 = variant("\xc3\xbc" "ber", 1);
        check("each non-ASCII UTF-8 byte becomes '-'", parse_variants(&utf8, 1) && (*parse_variants(&utf8, 1))[0].name == "--ber");
    }
    {
        EnhancementsVariant raw[17];
        for (EnhancementsVariant &v : raw)
            v = variant("a", 1);
        check("null, zero and seventeen variants are invalid, sixteen are not", !parse_variants(nullptr, 1) && !parse_variants(raw, 0)
            && !parse_variants(raw, 17) && parse_variants(raw, 16));
        EnhancementsVariant empty = variant("", 1);
        check("an empty name is invalid", !parse_variants(&empty, 1));
        EnhancementsVariant short_settings = variant("a", 1);
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
        const auto row = [](uint32_t index, const char *name, const char *line, Flicker f) {
            CompareRow r;
            r.index = index;
            r.name = name;
            r.line = line;
            r.flicker = f;
            r.size = { 1280, 800 };
            return r;
        };
        std::vector<CompareRow> rows = { row(0, "off", "off; comparing 0/2", { 0, 0, 0 }),
            row(1, "ambient", "1152x720 found gbuffer; ran ambient@combine", { 0.0123456, 40, 0.5 }),
            row(2, "traced", "1152x720 found gbuffer; ran trace@combine (64 ao, 32 sun)", { 0, 0, 0 }) };
        rows[2].trace_samples = 64;
        rows[2].trace_target = 64;
        rows[2].trace_wait = 412;
        std::vector<CompareRow> slow = { rows[2] };
        slow[0].trace_samples = 94;
        slow[0].trace_target = 128;
        slow[0].trace_wait = 3103;
        slow[0].trace_timeout = true;
        check("a variant saved short of its samples says so in its status", compare_tsv(slow).find("ran trace@combine (64 ao, 32 sun); gave up waiting for the "
            "trace at 94 of 128 samples after 3103 presents\t0.000000\t0\t0.000\t1280\t800\t94\t128\t3103\t1\n") != std::string::npos);
        check("compare.tsv has a header and one row per variant, with the trace's samples and wait", compare_tsv(rows)
            == "index\tname\tstatus\tflicker\tflicker_max\tflicker_mean\twidth\theight\ttrace_samples\ttrace_target\ttrace_wait\ttrace_timeout\n"
               "0\toff\toff; comparing 0/2\t0.000000\t0\t0.000\t1280\t800\t-\t-\t0\t0\n"
               "1\tambient\t1152x720 found gbuffer; ran ambient@combine\t0.012346\t40\t0.500\t1280\t800\t-\t-\t0\t0\n"
               "2\ttraced\t1152x720 found gbuffer; ran trace@combine (64 ao, 32 sun)\t0.000000\t0\t0.000\t1280\t800\t64\t64\t412\t0\n");
    }
    {
        EnhancementsVariant raw[] = { variant("off", 0), variant("shadows", 1) };
        raw[1].settings.view = ENHANCEMENTS_VIEW_SHADOW_MASK;
        raw[1].settings.contact_shadows = { 1, 1.2f, 0.7f, 0.3f, 0.4f };
        raw[1].settings.atmosphere = { 1, 0.001f, 0.05f, 2, 0.5f };
        raw[1].settings.tonemap = { 1, 0.5f, ENHANCEMENTS_CURVE_AGX, 1.5f, 0.9f, 1.2f };
        raw[1].settings.sun_shadows = { 1, 0.05f, 0.1f, 0.8f };
        raw[1].settings.wetness = { 1, 0.75f, 0.5f, 0.6f };
        raw[1].settings.light_shafts = { 1, 2, 0.02f, 25, 0.8f, 300, 0.25f, 12, 0.5f, 96 };
        raw[1].settings.trace = { 1, ENHANCEMENTS_TRACE_SUN, ENHANCEMENTS_TRACE_CAST_ALPHA_CARDS, 5, 0.5f, 0.03f, 0.75f, 16, 8, 30 };
        raw[1].settings.gather = { 1, 256, ENHANCEMENTS_GATHER_SKY_WEIGHTED, 1, 2, 0.25f };
        raw[1].settings.depth_of_field = { 1, 1.4f, 36, 3, { 0.25f, 0.75f }, 0, 0.05f, 3, 1, 15, -1, 0.6f, 0.3f, 1.33f, 1, 100, 4, 0, 0, 1, 1, 0.42f, 0.02f };
        raw[1].settings.lens_flare = { 1, ENHANCEMENTS_FLARE_SOURCE_MANUAL, { 1.2f, -0.1f }, 2, 0.5f, 1.5f, 0, 3, 1.25f, 0.9f, 2, 0.4f, 0.6f, ENHANCEMENTS_FLARE_PLACE_DISPLAY };
        check("settings.tsv writes every parsed field, one row per variant", settings_tsv(*parse_variants(raw, 2))
            == "index\tname\tenabled\tview\tflip\tambient.enabled\tambient.level\tambient.ao_strength\tambient.ao_radius"
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
               "\tlens_flare.size\tlens_flare.warmth\tlens_flare.chromatic\tlens_flare.visibility_seconds\tlens_flare.filled_arcs\tlens_flare.place\n"
               "0\toff\t0\toff\t1\t1\t0.6\t0.5\t1\t0.5\t0\t0.6\t1\t0.25\t0\t0\t0.000325\t0.02\t1\t0.7\t0\t0\tgame\t1\t1\t1"
               "\t0\t0.03\t0.02\t1\t0\t0\t0.35\t0.3\t0\t1\t0.01\t10\t0.6\t150\t0.5\t6\t1\t48"
               "\t0\t1\t1\t1\t1\t0\t2\t1\t0.0093\t1\t64\t32\t20\t0\t128\tvisibility\t2\t1\t0.5"
               "\t0\t2.8\t24\t0\t0.5\t0.5\t0.9\t0.03\t0\t0.5\t0\t0.6\t0\t0\t1\t0\t49\t2\t2\t1\t1\tscene\t0\t0.015"
               "\t0\tsun\t0.8\t0.12\t1\t1\t1\t1\t1\t1\t0.5\t1\t0.15\t1\tscene\n"
               "1\tshadows\t1\tshadow-mask\t1\t1\t0.6\t0.5\t1\t0.5\t1\t1.2\t0.7\t0.3\t0.4\t1\t0.001\t0.05\t2\t0.5\t1\t0.5\tagx\t1.5\t0.9\t1.2"
               "\t1\t0.05\t0.1\t0.8\t1\t0.75\t0.5\t0.6\t1\t2\t0.02\t25\t0.8\t300\t0.25\t12\t0.5\t96"
               "\t1\t0\t1\t0\t1\t0\t5\t0.5\t0.03\t0.75\t16\t8\t30\t1\t256\tweighted\t1\t2\t0.25"
               "\t1\t1.4\t36\t3\t0.25\t0.75\t0\t0.05\t5\t1\t15\t-1\t0.6\t0.3\t1.33\t1\t100\t4\t0\t0\t1\tdisplay\t0.42\t0.02"
               "\t1\tmanual\t1.2\t-0.1\t2\t0.5\t1.5\t0\t3\t1.25\t0.9\t2\t0.4\t0.6\tdisplay\n");
    }
    std::printf("%s: %d failed\n", __FILE__, failures);
    return failures == 0 ? 0 : 1;
}
