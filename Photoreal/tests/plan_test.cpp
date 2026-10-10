// The plan and the report: which passes run at which moment, what the status line says, how settings are parsed.
#include "fixture.h"
#include "../core/plan.h"
#include "../photoreal.h"

#include <cmath>
#include <cstdio>

using namespace photoreal;

namespace
{
    int failures = 0;

    void check(const char *name, bool ok)
    {
        std::printf("%s %s\n", ok ? "PASS" : "FAIL", name);
        failures += ok ? 0 : 1;
    }

    PhotorealSettings raw_settings()
    {
        PhotorealSettings raw {};
        raw.size = sizeof raw;
        raw.enabled = 1;
        raw.flip = 1;
        raw.ambient = { 1, 0.4f, 0.8f, 1.5f, 0.7f };
        return raw;
    }

    Settings ambient_on() { return parse_settings(raw_settings()); }

    Settings contact_shadows_on(bool ambient, uint32_t view = PHOTOREAL_VIEW_OFF)
    {
        PhotorealSettings raw = raw_settings();
        raw.ambient.enabled = ambient;
        raw.contact_shadows = { 1, 0.6f, 1, 0.25f, 0 };
        raw.view = view;
        return parse_settings(raw);
    }

    Settings sun_shadows_on(bool contact_shadows)
    {
        PhotorealSettings raw = raw_settings();
        raw.contact_shadows = { contact_shadows, 0.6f, 1, 0.25f, 0 };
        raw.sun_shadows = { 1, 0.03f, 0.02f, 1 };
        return parse_settings(raw);
    }

    Settings atmosphere_on()
    {
        PhotorealSettings raw = raw_settings();
        raw.ambient.enabled = 0;
        raw.atmosphere = { 1, 0.000325f, 0.02f, 1, 0.7f };
        return parse_settings(raw);
    }

    Settings tonemap_on(bool atmosphere)
    {
        PhotorealSettings raw = raw_settings();
        raw.ambient.enabled = 0;
        raw.atmosphere = { atmosphere, 0.000325f, 0.02f, 1, 0.7f };
        raw.tonemap = { 1, 0, PHOTOREAL_CURVE_GAME, 1, 1, 1 };
        return parse_settings(raw);
    }

    Settings view_only(uint32_t view)
    {
        PhotorealSettings raw = raw_settings();
        raw.view = view;
        raw.ambient.enabled = 0;
        return parse_settings(raw);
    }

    // Replays a fixture as the shell does: at every moment, plan the passes, note the plan and the view; finish at the end.
    FrameReport run_frame(std::vector<fixture::Row> &rows, const Settings &settings, const Inputs &inputs, std::vector<Step> *snapshots = nullptr)
    {
        FrameTracker tracker;
        FrameReport report = FrameReport::start(settings);
        tracker.begin_frame();
        for (fixture::Row &row : rows)
        {
            const std::optional<Step> step = tracker.feed(fixture::event(row));
            if (!step)
                continue;
            report.note(plan(*step, tracker.map(), settings, inputs));
            if (report.note_view(*step, tracker.map()) && snapshots)
                snapshots->push_back(*step);
        }
        report.finish(tracker, settings, inputs);
        return report;
    }

    // A camera, and sun constants that passed their checks this frame.
    const Inputs kCamera { true, SunCheck::ok };
    const Inputs kNoCamera { false, SunCheck::ok };

    const char *kAllSteps = "1152x720 found gbuffer quarter-shadow shadow-mask ambient-pair combine bloom tonemap forward";
}

int main()
{
    const std::vector<fixture::Row> walk = fixture::load("fixtures/capture-20261009-214859.tsv");
    {
        auto rows = walk;
        const FrameReport report = run_frame(rows, ambient_on(), kCamera);
        check("ambient runs at combine", report.ran.bits == PHOTOREAL_PASS_AMBIENT && report.skipped.empty());
        check("describe names every step and the pass", describe(report) == std::string(kAllSteps) + "; ran ambient@combine");
        check("steps wanted by ambient: gbuffer and combine", report.wanted.bits == (PHOTOREAL_STEP_GBUFFER | PHOTOREAL_STEP_COMBINE));
    }
    {
        auto rows = walk;
        const FrameReport report = run_frame(rows, contact_shadows_on(true), kCamera);
        check("contact shadows and ambient both run", report.ran.bits == (PHOTOREAL_PASS_CONTACT_SHADOWS | PHOTOREAL_PASS_AMBIENT)
            && report.skipped.empty());
        check("describe lists contact shadows before ambient at combine",
            describe(report) == std::string(kAllSteps) + "; ran contact-shadows@combine ambient@combine");
        check("steps wanted by both: gbuffer and combine", report.wanted.bits == (PHOTOREAL_STEP_GBUFFER | PHOTOREAL_STEP_COMBINE));
    }
    {
        auto rows = walk;
        FrameTracker tracker;
        fixture::replay(rows, tracker);
        const Plan p = plan(Step::combine, tracker.map(), contact_shadows_on(true), kCamera);
        check("the combine plan holds sun shadows, off, then runs contact shadows, then ambient", p.count == 3
            && p.items[0].pass == PassId::sun_shadows && p.items[0].kind == Decision::Kind::off
            && p.items[1].pass == PassId::contact_shadows && p.items[1].kind == Decision::Kind::run
            && p.items[2].pass == PassId::ambient && p.items[2].kind == Decision::Kind::run);
        const Plan at_tonemap = plan(Step::tonemap, tracker.map(), contact_shadows_on(true), kCamera);
        check("the shadow-mask step has no pass, and the tonemap step only tonemap, off", plan(Step::shadow_mask, tracker.map(), contact_shadows_on(true), kCamera).count == 0
            && at_tonemap.count == 1 && at_tonemap.items[0].pass == PassId::tonemap && at_tonemap.items[0].kind == Decision::Kind::off);
    }
    {
        auto rows = walk;
        const FrameReport report = run_frame(rows, sun_shadows_on(true), kCamera);
        check("sun shadows run at combine before contact shadows and ambient", report.ran.bits
            == (PHOTOREAL_PASS_SUN_SHADOWS | PHOTOREAL_PASS_CONTACT_SHADOWS | PHOTOREAL_PASS_AMBIENT) && describe(report)
            == std::string(kAllSteps) + "; ran sun-shadows@combine contact-shadows@combine ambient@combine");
        check("steps wanted by sun shadows: gbuffer and combine", report.wanted.bits == (PHOTOREAL_STEP_GBUFFER | PHOTOREAL_STEP_COMBINE));
        FrameTracker tracker;
        fixture::replay(rows, tracker);
        const Plan p = plan(Step::combine, tracker.map(), sun_shadows_on(true), kCamera);
        check("the combine plan runs sun shadows first", p.count == 3 && p.items[0].pass == PassId::sun_shadows
            && p.items[0].kind == Decision::Kind::run && p.items[1].pass == PassId::contact_shadows);
    }
    {
        auto rows = walk;
        const FrameReport report = run_frame(rows, sun_shadows_on(false), Inputs { true, SunCheck::unread });
        check("before any constants were read back, sun shadows skip and say so, and ambient runs", report.skipped.bits
            == PHOTOREAL_PASS_SUN_SHADOWS && report.ran.bits == PHOTOREAL_PASS_AMBIENT && report.missing.empty() && describe(report)
            == std::string(kAllSteps) + "; ran ambient@combine; skipped sun-shadows: sun constants unread");
    }
    {
        auto rows = walk;
        const FrameReport report = run_frame(rows, sun_shadows_on(true), Inputs { false, SunCheck::radii_order });
        check("constants that failed a check and no camera: both reasons", describe(report) == std::string(kAllSteps)
            + "; skipped sun-shadows: no camera, sun constants radii-order; skipped contact-shadows: no camera; skipped ambient: no camera");
    }
    {
        auto rows = walk;
        fixture::find(rows, 893).draw.inputs.resize(2);
        const FrameReport report = run_frame(rows, sun_shadows_on(false), Inputs { true, SunCheck::unread });
        check("a shadow-mask draw without the atlas at t2: sun shadows miss sun-atlas", report.missing.bits == PHOTOREAL_ENTRY_SUN_ATLAS
            && describe(report) == std::string(kAllSteps) + "; ran ambient@combine; skipped sun-shadows: missing sun-atlas, sun constants unread");
    }
    {
        std::vector<fixture::Row> menu;
        const FrameReport report = run_frame(menu, sun_shadows_on(false), Inputs { true, SunCheck::unread });
        check("no G-buffer: sun shadows name the entries they need", describe(report)
            == "found nothing; skipped sun-shadows: missing normals depth shadow-mask sun-atlas, sun constants unread; skipped ambient: missing normals depth ambient-diffuse");
    }
    {
        auto rows = walk;
        const FrameReport report = run_frame(rows, atmosphere_on(), kCamera);
        check("atmosphere runs at bloom", describe(report) == std::string(kAllSteps) + "; ran atmosphere@bloom");
        check("steps wanted by atmosphere: gbuffer, combine and bloom",
            report.wanted.bits == (PHOTOREAL_STEP_GBUFFER | PHOTOREAL_STEP_COMBINE | PHOTOREAL_STEP_BLOOM));
        FrameTracker tracker;
        fixture::replay(rows, tracker);
        const Plan at_bloom = plan(Step::bloom, tracker.map(), atmosphere_on(), kCamera);
        check("the bloom plan runs atmosphere alone", at_bloom.count == 1 && at_bloom.items[0].pass == PassId::atmosphere
            && at_bloom.items[0].kind == Decision::Kind::run);
        check("the combine plan holds sun shadows, contact shadows and ambient, all off", plan(Step::combine, tracker.map(), atmosphere_on(), kCamera).count == 3
            && plan(Step::combine, tracker.map(), atmosphere_on(), kCamera).items[0].kind == Decision::Kind::off);
    }
    {
        auto rows = walk;
        const FrameReport report = run_frame(rows, atmosphere_on(), kNoCamera);
        check("without a camera, atmosphere skips and says so", report.skipped.bits == PHOTOREAL_PASS_ATMOSPHERE
            && describe(report) == std::string(kAllSteps) + "; skipped atmosphere: no camera");
    }
    {
        auto rows = walk;
        fixture::drop(rows, 1088, 1088);  // the first bloom draw, the one that samples the HDR scene
        const FrameReport report = run_frame(rows, atmosphere_on(), kCamera);
        check("no bloom moment: atmosphere skips and names the step", describe(report)
            == "1152x720 found gbuffer quarter-shadow shadow-mask ambient-pair combine tonemap forward; skipped atmosphere: no bloom");
    }
    {
        std::vector<fixture::Row> menu;
        const FrameReport report = run_frame(menu, atmosphere_on(), kCamera);
        check("no G-buffer: atmosphere names the entries it needs", describe(report) == "found nothing; skipped atmosphere: missing depth hdr-scene"
            && report.missing.bits == (PHOTOREAL_ENTRY_DEPTH | PHOTOREAL_ENTRY_HDR_SCENE));
    }
    {
        auto rows = walk;
        const FrameReport report = run_frame(rows, tonemap_on(true), kCamera);
        check("atmosphere runs at bloom and tonemap at tonemap", describe(report) == std::string(kAllSteps) + "; ran atmosphere@bloom tonemap@tonemap");
        check("steps wanted by atmosphere and tonemap: gbuffer, combine, bloom and tonemap",
            report.wanted.bits == (PHOTOREAL_STEP_GBUFFER | PHOTOREAL_STEP_COMBINE | PHOTOREAL_STEP_BLOOM | PHOTOREAL_STEP_TONEMAP));
        FrameTracker tracker;
        fixture::replay(rows, tracker);
        const Plan at_tonemap = plan(Step::tonemap, tracker.map(), tonemap_on(false), kNoCamera);
        check("the tonemap plan runs tonemap alone, without a camera", at_tonemap.count == 1 && at_tonemap.items[0].pass == PassId::tonemap
            && at_tonemap.items[0].kind == Decision::Kind::run);
    }
    {
        auto rows = walk;
        const FrameReport report = run_frame(rows, tonemap_on(true), kNoCamera);
        check("without a camera, tonemap still runs and atmosphere skips",
            describe(report) == std::string(kAllSteps) + "; ran tonemap@tonemap; skipped atmosphere: no camera");
    }
    {
        auto rows = walk;
        fixture::find(rows, 1118).draw.inputs.resize(1);
        const FrameReport report = run_frame(rows, tonemap_on(false), kCamera);
        check("without a final bloom, tonemap still runs", report.ran.bits == PHOTOREAL_PASS_TONEMAP && report.missing.empty());
    }
    {
        auto rows = walk;
        fixture::drop(rows, 1100, 1200);  // everything from the tonemap on
        const FrameReport report = run_frame(rows, tonemap_on(false), kCamera);
        check("no tonemap draw: tonemap skips for want of the output it draws into", describe(report)
            == "1152x720 found gbuffer quarter-shadow shadow-mask ambient-pair combine bloom forward; skipped tonemap: missing tonemap-out");
    }
    {
        std::vector<fixture::Row> menu;
        const FrameReport report = run_frame(menu, tonemap_on(true), kCamera);
        check("no G-buffer: atmosphere and tonemap name the entries they need", describe(report)
            == "found nothing; skipped atmosphere: missing depth hdr-scene; skipped tonemap: missing hdr-scene tonemap-out");
    }
    {
        auto rows = fixture::load("fixtures/capture-20261010-111824.tsv");
        const FrameReport report = run_frame(rows, contact_shadows_on(true), kCamera);
        check("111824, the user's normal settings: contact shadows and ambient both run", describe(report)
            == "1920x1200 found gbuffer quarter-shadow shadow-mask ambient-pair combine bloom tonemap forward; ran contact-shadows@combine ambient@combine");
    }
    {
        auto rows = walk;
        fixture::drop(rows, 892, 892);  // the shadow mask's clear, so no shadow mask is found
        const FrameReport report = run_frame(rows, contact_shadows_on(true), kCamera);
        check("without the shadow mask, contact shadows skip and ambient runs",
            report.skipped.bits == PHOTOREAL_PASS_CONTACT_SHADOWS && report.ran.bits == PHOTOREAL_PASS_AMBIENT);
        check("and the shadow mask is the missing entry", report.missing.bits == PHOTOREAL_ENTRY_SHADOW_MASK);
        check("describe says why", describe(report)
            == "1152x720 found gbuffer quarter-shadow ambient-pair combine bloom tonemap forward; ran ambient@combine; skipped contact-shadows: missing shadow-mask");
    }
    {
        auto rows = walk;
        const FrameReport report = run_frame(rows, contact_shadows_on(true), kNoCamera);
        check("without a camera, both passes skip and say so", report.skipped.bits == (PHOTOREAL_PASS_CONTACT_SHADOWS | PHOTOREAL_PASS_AMBIENT)
            && describe(report) == std::string(kAllSteps) + "; skipped contact-shadows: no camera; skipped ambient: no camera");
    }
    {
        std::vector<fixture::Row> menu;
        const FrameReport report = run_frame(menu, contact_shadows_on(false), kCamera);
        check("no G-buffer: contact shadows name the entries they need",
            describe(report) == "found nothing; skipped contact-shadows: missing normals depth shadow-mask");
    }
    {
        auto rows = walk;
        std::vector<Step> snapshots;
        const FrameReport report = run_frame(rows, contact_shadows_on(false, PHOTOREAL_VIEW_SHADOW_MASK), kCamera, &snapshots);
        check("the shadow-mask view snapshots at combine, the moment contact shadows run",
            snapshots == std::vector<Step> { Step::combine } && report.ran.bits == PHOTOREAL_PASS_CONTACT_SHADOWS);
        check("describe names the pass and the view", describe(report) == std::string(kAllSteps) + "; ran contact-shadows@combine; view shadow-mask");
    }
    {
        auto rows = walk;
        fixture::drop(rows, 910, 913);
        const FrameReport report = run_frame(rows, ambient_on(), kCamera);
        check("without the ambient pair, ambient skips", report.skipped.bits == PHOTOREAL_PASS_AMBIENT && report.ran.empty());
        check("and reports the missing entry", report.missing.bits == PHOTOREAL_ENTRY_AMBIENT_DIFFUSE);
        check("describe says why",
            describe(report) == "1152x720 found gbuffer quarter-shadow shadow-mask combine bloom tonemap forward; skipped ambient: missing ambient-diffuse");
    }
    {
        auto rows = walk;
        const FrameReport report = run_frame(rows, ambient_on(), kNoCamera);
        check("without a camera, ambient skips and says so", report.skipped.bits == PHOTOREAL_PASS_AMBIENT
            && describe(report) == std::string(kAllSteps) + "; skipped ambient: no camera");
    }
    {
        std::vector<fixture::Row> menu;  // a frame with no G-buffer: a loading screen or a menu
        const FrameReport report = run_frame(menu, ambient_on(), kCamera);
        check("no G-buffer: nothing runs", report.ran.empty() && report.skipped.bits == PHOTOREAL_PASS_AMBIENT);
        check("no G-buffer: describe says so", describe(report) == "found nothing; skipped ambient: missing normals depth ambient-diffuse");
        check("no G-buffer: status bits name the missing entries",
            report.missing.bits == (PHOTOREAL_ENTRY_NORMALS | PHOTOREAL_ENTRY_DEPTH | PHOTOREAL_ENTRY_AMBIENT_DIFFUSE));
    }
    {
        auto rows = walk;
        fixture::drop(rows, 924, 926);  // the HDR scene's bind and clear: every entry ambient needs, but no combine moment
        const FrameReport report = run_frame(rows, ambient_on(), kCamera);
        check("no combine moment: ambient skips and names the step",
            describe(report) == "1152x720 found gbuffer quarter-shadow shadow-mask ambient-pair; skipped ambient: no combine");
    }
    {
        auto rows = walk;
        std::vector<Step> snapshots;
        const FrameReport report = run_frame(rows, view_only(PHOTOREAL_VIEW_NORMALS), kCamera, &snapshots);
        check("the normals view snapshots once, at combine", snapshots == std::vector<Step> { Step::combine });
        check("the normals view is shown", report.view_shown() == View::normals);
        check("describe names the view", describe(report) == std::string(kAllSteps) + "; view normals");
    }
    {
        auto rows = walk;
        std::vector<Step> snapshots;
        const FrameReport report = run_frame(rows, view_only(PHOTOREAL_VIEW_HDR_SCENE), kCamera, &snapshots);
        check("the HDR view snapshots at tonemap", snapshots == std::vector<Step> { Step::tonemap } && report.view_shown() == View::hdr_scene);
        check("the HDR view wants the steps up to tonemap",
            report.wanted.bits == (PHOTOREAL_STEP_GBUFFER | PHOTOREAL_STEP_COMBINE | PHOTOREAL_STEP_TONEMAP));
    }
    {
        auto rows = fixture::load("fixtures/capture-20261010-111824.tsv");
        std::vector<Step> snapshots;
        const FrameReport report = run_frame(rows, view_only(PHOTOREAL_VIEW_BLOOM_FINAL), kCamera, &snapshots);
        check("the bloom-final view snapshots at tonemap and is shown",
            snapshots == std::vector<Step> { Step::tonemap } && report.view_shown() == View::bloom_final);
        check("describe names the bloom-final view", describe(report)
            == "1920x1200 found gbuffer quarter-shadow shadow-mask ambient-pair combine bloom tonemap forward; view bloom-final");
    }
    {
        auto rows = fixture::load("fixtures/capture-20261010-111824.tsv");
        std::vector<Step> snapshots;
        const FrameReport report = run_frame(rows, view_only(PHOTOREAL_VIEW_SUN_ATLAS), kCamera, &snapshots);
        check("the sun-atlas view snapshots at the shadow-mask draw and is shown",
            snapshots == std::vector<Step> { Step::shadow_mask } && report.view_shown() == View::sun_atlas);
        check("the sun-atlas view wants the gbuffer and shadow-mask steps",
            report.wanted.bits == (PHOTOREAL_STEP_GBUFFER | PHOTOREAL_STEP_SHADOW_MASK));
        check("describe names the sun-atlas view", describe(report)
            == "1920x1200 found gbuffer quarter-shadow shadow-mask ambient-pair combine bloom tonemap forward; view sun-atlas");
    }
    {
        auto rows = walk;
        fixture::find(rows, 1118).draw.inputs.resize(1);
        const FrameReport report = run_frame(rows, view_only(PHOTOREAL_VIEW_BLOOM_FINAL), kCamera);
        check("without a final bloom at the tonemap draw, the view says it is missing", report.view_shown() == View::off
            && report.missing.bits == PHOTOREAL_ENTRY_BLOOM_FINAL && describe(report) == std::string(kAllSteps) + "; view bloom-final: missing bloom-final");
    }
    {
        auto rows = walk;
        fixture::drop(rows, 884, 886);  // the quarter shadow's bind and draw
        const FrameReport report = run_frame(rows, view_only(PHOTOREAL_VIEW_QUARTER_SHADOW), kCamera);
        check("a view of a missing entry shows nothing and says what is missing", report.view_shown() == View::off
            && report.missing.bits == PHOTOREAL_ENTRY_QUARTER_SHADOW
            && describe(report) == "1152x720 found gbuffer shadow-mask ambient-pair combine bloom tonemap forward; view quarter-shadow: missing quarter-shadow");
    }
    {
        auto rows = walk;
        fixture::drop(rows, 1100, 1200);  // everything from the tonemap on
        const FrameReport report = run_frame(rows, view_only(PHOTOREAL_VIEW_HDR_SCENE), kCamera);
        check("a view whose step never came names the step", report.view_shown() == View::off && report.missing.empty()
            && describe(report) == "1152x720 found gbuffer quarter-shadow shadow-mask ambient-pair combine bloom forward; view hdr-scene: no tonemap");
    }
    {
        const auto glyphs = [](const Label &label) { return std::vector<int>(label.glyphs.begin(), label.glyphs.begin() + label.length); };
        check("the shadow-mask label spells SHADOW-MASK in font glyphs",
            glyphs(view_label(View::shadow_mask, true)) == std::vector<int> { 19, 8, 1, 4, 15, 23, 37, 13, 1, 19, 11 });
        check("a missing view's label adds a space and MISSING",
            glyphs(view_label(View::hdr_scene, false)) == std::vector<int> { 8, 4, 18, 37, 19, 3, 5, 14, 5, 0, 13, 9, 19, 19, 9, 14, 7 });
        check("digits, either case, and an unknown character as '?'", glyphs(encode_label("Ab09_")) == std::vector<int> { 1, 2, 27, 36, 38 });
        check("a label stops at 32 glyphs", encode_label(std::string(40, 'z')).length == 32);
    }
    {
        FrameReport report = FrameReport::start(parse_settings(PhotorealSettings { sizeof(PhotorealSettings), 0, 0, 1, {}, {}, {}, {}, {} }));
        check("disabled: describe says off", describe(report) == "off");
        report.error = PHOTOREAL_ERROR_SHADER;
        check("disabled by an error: describe says which", describe(report) == "off; error shader");
        report.compare_saved = 1;
        report.compare_count = 3;
        check("a comparison capture's progress comes before the error", describe(report) == "off; comparing 1/3; error shader");
    }
    {
        auto rows = walk;
        FrameReport report = run_frame(rows, ambient_on(), kCamera);
        report.compare_count = 16;
        check("a capture's progress ends an armed line", describe(report) == std::string(kAllSteps) + "; ran ambient@combine; comparing 0/16");
    }
    {
        auto rows = walk;
        FrameReport report = run_frame(rows, ambient_on(), kCamera);
        report.note_failed(PassId::ambient);
        report.error = PHOTOREAL_ERROR_TEXTURE;
        check("a pass whose texture failed is skipped",
            describe(report) == std::string(kAllSteps) + "; skipped ambient: no texture; error texture");
    }
    {
        PhotorealSettings raw = raw_settings();
        raw.size = 8;  // a caller that only knows size and enabled
        raw.view = 3;
        raw.flip = 0;
        raw.ambient = { 1, 2, 0.25f, 2, 0.75f };
        const Settings s = parse_settings(raw);
        check("a short struct reads what it holds", s.enabled);
        check("a short struct keeps later fields at their defaults: level 0.6, strength 0.5, radius 1, foliage 0.5",
            s.view == View::off && s.flip && !s.ambient.enabled && s.ambient.level == 0.6f && s.ambient.ao_strength == 0.5f
            && s.ambient.ao_radius == 1.0f && s.ambient.foliage_ao_strength == 0.5f);
        raw.size = 4;
        check("a struct too short for enabled reads as off", !parse_settings(raw).enabled);
        raw.size = 32;  // a caller built before foliage_ao_strength existed
        const Settings v1 = parse_settings(raw);
        check("a 32-byte struct reads its ambient block and keeps foliage strength at 0.5", v1.ambient.enabled
            && v1.ambient.level == 2.0f && v1.ambient.ao_strength == 0.25f && v1.ambient.ao_radius == 2.0f
            && v1.ambient.foliage_ao_strength == 0.5f);
        raw.size = sizeof raw;
        check("a whole struct reads foliage strength 0.75", parse_settings(raw).ambient.foliage_ao_strength == 0.75f);
    }
    {
        PhotorealSettings raw = raw_settings();
        raw.view = 99;
        raw.flip = 0;
        raw.ambient = { 1, NAN, -3, INFINITY, NAN };
        const Settings s = parse_settings(raw);
        check("an unknown view reads as off", s.view == View::off);
        check("flip 0 reads as upright", !s.flip);
        check("NaN and infinity fall back to the defaults, and -3 clamps to 0", s.ambient.level == 0.6f
            && s.ambient.ao_strength == 0.0f && s.ambient.ao_radius == 1.0f && s.ambient.foliage_ao_strength == 0.5f);
        raw.ambient = { 1, 9, 2, 0.001f, 3 };
        const Settings c = parse_settings(raw);
        check("out-of-range floats clamp: level 4, strength 1, radius 0.05, foliage 1", c.ambient.level == 4.0f
            && c.ambient.ao_strength == 1.0f && c.ambient.ao_radius == 0.05f && c.ambient.foliage_ao_strength == 1.0f);
        raw.ambient.foliage_ao_strength = -INFINITY;
        check("foliage strength -infinity falls back to 0.5", parse_settings(raw).ambient.foliage_ao_strength == 0.5f);
        raw.ambient.foliage_ao_strength = -1;
        check("foliage strength -1 clamps to 0", parse_settings(raw).ambient.foliage_ao_strength == 0.0f);
    }
    {
        PhotorealSettings raw = raw_settings();
        raw.contact_shadows = { 1, 1.2f, 0.7f, 0.3f, 0.4f };
        const Settings whole = parse_settings(raw);
        check("a whole struct reads contact shadows: on, length 1.2, strength 0.7, thickness 0.3, foliage 0.4", whole.contact_shadows.enabled
            && whole.contact_shadows.length == 1.2f && whole.contact_shadows.strength == 0.7f && whole.contact_shadows.thickness == 0.3f
            && whole.contact_shadows.foliage_strength == 0.4f);
        raw.size = 52;  // a caller built before foliage_strength existed
        const Settings v2 = parse_settings(raw);
        check("a 52-byte struct reads its contact-shadow block and keeps foliage strength at 0", v2.contact_shadows.enabled
            && v2.contact_shadows.length == 1.2f && v2.contact_shadows.strength == 0.7f && v2.contact_shadows.thickness == 0.3f
            && v2.contact_shadows.foliage_strength == 0.0f);
        raw.size = 55;
        check("a struct one byte short of foliage strength keeps it at 0", parse_settings(raw).contact_shadows.foliage_strength == 0.0f
            && parse_settings(raw).contact_shadows.thickness == 0.3f);
        raw.size = 36;  // a caller built before contact shadows existed
        const Settings v1 = parse_settings(raw);
        check("a 36-byte struct reads its ambient block and keeps contact shadows off at length 0.6, strength 1, thickness 0.25",
            v1.ambient.enabled && v1.ambient.foliage_ao_strength == 0.7f && !v1.contact_shadows.enabled && v1.contact_shadows.length == 0.6f
                && v1.contact_shadows.strength == 1.0f && v1.contact_shadows.thickness == 0.25f && v1.contact_shadows.foliage_strength == 0.0f);
        raw.size = 51;
        check("a struct one byte short of the contact-shadow block keeps its defaults", !parse_settings(raw).contact_shadows.enabled
            && parse_settings(raw).contact_shadows.thickness == 0.25f);
        raw.size = sizeof raw;
        raw.contact_shadows = { 1, NAN, INFINITY, -INFINITY, NAN };
        const Settings bad = parse_settings(raw);
        check("contact shadows: NaN and infinities fall back to length 0.6, strength 1, thickness 0.25, foliage 0", bad.contact_shadows.length == 0.6f
            && bad.contact_shadows.strength == 1.0f && bad.contact_shadows.thickness == 0.25f && bad.contact_shadows.foliage_strength == 0.0f);
        raw.contact_shadows = { 1, 0.01f, -1, 5, -1 };
        const Settings low = parse_settings(raw);
        check("contact shadows clamp: length 0.01 to 0.05, strength -1 to 0, thickness 5 to 2, foliage -1 to 0", low.contact_shadows.length == 0.05f
            && low.contact_shadows.strength == 0.0f && low.contact_shadows.thickness == 2.0f && low.contact_shadows.foliage_strength == 0.0f);
        raw.contact_shadows = { 1, 9, 2, 0.001f, 3 };
        const Settings high = parse_settings(raw);
        check("contact shadows clamp: length 9 to 5, strength 2 to 1, thickness 0.001 to 0.01, foliage 3 to 1", high.contact_shadows.length == 5.0f
            && high.contact_shadows.strength == 1.0f && high.contact_shadows.thickness == 0.01f && high.contact_shadows.foliage_strength == 1.0f);
    }
    {
        PhotorealSettings raw = raw_settings();
        raw.atmosphere = { 1, 0.002f, 0.05f, 3, -0.2f };
        const Settings whole = parse_settings(raw);
        check("a whole struct reads atmosphere: on, density 0.002, falloff 0.05, sun scatter 3, anisotropy -0.2", whole.atmosphere.enabled
            && whole.atmosphere.density == 0.002f && whole.atmosphere.height_falloff == 0.05f && whole.atmosphere.sun_scatter == 3.0f
            && whole.atmosphere.anisotropy == -0.2f);
        raw.size = 56;  // a caller built before atmosphere existed
        const Settings v3 = parse_settings(raw);
        check("a 56-byte struct keeps atmosphere off at density 0.000325, falloff 0.02, sun scatter 1, anisotropy 0.7",
            !v3.atmosphere.enabled && v3.atmosphere.density == 0.000325f && v3.atmosphere.height_falloff == 0.02f
                && v3.atmosphere.sun_scatter == 1.0f && v3.atmosphere.anisotropy == 0.7f && v3.contact_shadows.foliage_strength == 0.0f);
        raw.size = 75;
        check("a struct one byte short of the atmosphere block keeps its defaults", !parse_settings(raw).atmosphere.enabled
            && parse_settings(raw).atmosphere.density == 0.000325f);
        raw.size = sizeof raw;
        raw.atmosphere = { 1, NAN, INFINITY, -INFINITY, NAN };
        const Settings bad = parse_settings(raw);
        check("atmosphere: NaN and infinities fall back to density 0.000325, falloff 0.02, sun scatter 1, anisotropy 0.7",
            bad.atmosphere.density == 0.000325f && bad.atmosphere.height_falloff == 0.02f && bad.atmosphere.sun_scatter == 1.0f
                && bad.atmosphere.anisotropy == 0.7f);
        raw.atmosphere = { 1, -1, -1, -1, -1 };
        const Settings low = parse_settings(raw);
        check("atmosphere clamps: density, falloff and sun scatter -1 to 0, anisotropy -1 to -0.95", low.atmosphere.density == 0.0f
            && low.atmosphere.height_falloff == 0.0f && low.atmosphere.sun_scatter == 0.0f && low.atmosphere.anisotropy == -0.95f);
        raw.atmosphere = { 1, 1, 2, 20, 1 };
        const Settings high = parse_settings(raw);
        check("atmosphere clamps: density 1 to 0.01, falloff 2 to 1, sun scatter 20 to 10, anisotropy 1 to 0.95", high.atmosphere.density == 0.01f
            && high.atmosphere.height_falloff == 1.0f && high.atmosphere.sun_scatter == 10.0f && high.atmosphere.anisotropy == 0.95f);
    }
    {
        PhotorealSettings raw = raw_settings();
        raw.tonemap = { 1, -1.5f, PHOTOREAL_CURVE_NEUTRAL, 0.5f, 1.2f, 1.1f };
        const Settings whole = parse_settings(raw);
        check("a whole struct reads tonemap: on, -1.5 EV, neutral, bloom 0.5, saturation 1.2, contrast 1.1", whole.tonemap.enabled
            && whole.tonemap.exposure_ev == -1.5f && whole.tonemap.curve == Curve::neutral && whole.tonemap.bloom_strength == 0.5f
            && whole.tonemap.saturation == 1.2f && whole.tonemap.contrast == 1.1f);
        raw.size = 76;  // a caller built before tonemap existed
        const Settings v3 = parse_settings(raw);
        check("a 76-byte struct keeps tonemap off at 0 EV, the game's curve, bloom 1, saturation 1, contrast 1", !v3.tonemap.enabled
            && v3.tonemap.exposure_ev == 0.0f && v3.tonemap.curve == Curve::game && v3.tonemap.bloom_strength == 1.0f
            && v3.tonemap.saturation == 1.0f && v3.tonemap.contrast == 1.0f);
        raw.size = 99;
        check("a struct one byte short of the tonemap block keeps its defaults", !parse_settings(raw).tonemap.enabled
            && parse_settings(raw).tonemap.curve == Curve::game);
        raw.size = sizeof raw;
        raw.tonemap = { 1, NAN, 7, INFINITY, NAN, -INFINITY };
        const Settings bad = parse_settings(raw);
        check("tonemap: NaN and infinities fall back to 0 EV, bloom 1, saturation 1, contrast 1, and curve 7 reads as the game's",
            bad.tonemap.exposure_ev == 0.0f && bad.tonemap.curve == Curve::game && bad.tonemap.bloom_strength == 1.0f
                && bad.tonemap.saturation == 1.0f && bad.tonemap.contrast == 1.0f);
        raw.tonemap = { 1, -20, PHOTOREAL_CURVE_AGX, -1, -1, 0.1f };
        const Settings low = parse_settings(raw);
        check("tonemap clamps: -20 EV to -10, bloom and saturation -1 to 0, contrast 0.1 to 0.5, and keeps AgX", low.tonemap.exposure_ev == -10.0f
            && low.tonemap.curve == Curve::agx && low.tonemap.bloom_strength == 0.0f && low.tonemap.saturation == 0.0f && low.tonemap.contrast == 0.5f);
        raw.tonemap = { 1, 20, PHOTOREAL_CURVE_GAME, 9, 5, 3 };
        const Settings high = parse_settings(raw);
        check("tonemap clamps: 20 EV to 10, bloom 9 to 4, saturation 5 to 2, contrast 3 to 2", high.tonemap.exposure_ev == 10.0f
            && high.tonemap.bloom_strength == 4.0f && high.tonemap.saturation == 2.0f && high.tonemap.contrast == 2.0f);
    }
    {
        PhotorealSettings raw = raw_settings();
        raw.sun_shadows = { 1, 0.05f, 0.1f, 0.8f };
        const Settings whole = parse_settings(raw);
        check("a whole struct reads sun shadows: on, light size 0.05, min penumbra 0.1, strength 0.8", whole.sun_shadows.enabled
            && whole.sun_shadows.light_size == 0.05f && whole.sun_shadows.min_penumbra == 0.1f && whole.sun_shadows.strength == 0.8f);
        raw.size = 100;  // a caller built before sun shadows existed
        const Settings v4 = parse_settings(raw);
        check("a 100-byte struct keeps sun shadows off at light size 0.03, min penumbra 0.02, strength 1", !v4.sun_shadows.enabled
            && v4.sun_shadows.light_size == 0.03f && v4.sun_shadows.min_penumbra == 0.02f && v4.sun_shadows.strength == 1.0f
            && v4.tonemap.curve == Curve::game);
        raw.size = 115;
        check("a struct one byte short of the sun-shadow block keeps its defaults", !parse_settings(raw).sun_shadows.enabled
            && parse_settings(raw).sun_shadows.light_size == 0.03f);
        raw.size = sizeof raw;
        raw.sun_shadows = { 1, NAN, INFINITY, -INFINITY };
        const Settings bad = parse_settings(raw);
        check("sun shadows: NaN and infinities fall back to light size 0.03, min penumbra 0.02, strength 1",
            bad.sun_shadows.light_size == 0.03f && bad.sun_shadows.min_penumbra == 0.02f && bad.sun_shadows.strength == 1.0f);
        raw.sun_shadows = { 1, -1, -1, -1 };
        const Settings low = parse_settings(raw);
        check("sun shadows clamp: light size, min penumbra and strength -1 to 0", low.sun_shadows.light_size == 0.0f
            && low.sun_shadows.min_penumbra == 0.0f && low.sun_shadows.strength == 0.0f);
        raw.sun_shadows = { 1, 1, 2, 3 };
        const Settings high = parse_settings(raw);
        check("sun shadows clamp: light size 1 to 0.2, min penumbra 2 to 0.5, strength 3 to 1", high.sun_shadows.light_size == 0.2f
            && high.sun_shadows.min_penumbra == 0.5f && high.sun_shadows.strength == 1.0f);
    }
    {
        PhotorealCamera raw {};
        raw.world_to_view[5] = 2;
        raw.view_to_clip[14] = 0.1f;
        raw.sun_direction[1] = 3;
        raw.sun_direction[2] = 4;
        raw.sun_direction[3] = 7;
        const Camera c = parse_camera(raw);
        check("the camera's matrices are copied", c.world_to_view[5] == 2.0f && c.view_to_clip[14] == 0.1f);
        check("the sun direction (0, 3, 4) normalizes to (0, 0.6, 0.8), and w is ignored",
            c.toward_sun[0] == 0.0f && c.toward_sun[1] == 0.6f && c.toward_sun[2] == 0.8f);
        raw.sun_direction[1] = raw.sun_direction[2] = 0;
        const Camera none = parse_camera(raw);
        check("a zero sun direction stays zero", none.toward_sun[0] == 0.0f && none.toward_sun[1] == 0.0f && none.toward_sun[2] == 0.0f);
        raw.sun_direction[0] = NAN;
        raw.sun_direction[1] = 1;
        const Camera nan = parse_camera(raw);
        check("a NaN sun direction becomes zero", nan.toward_sun[0] == 0.0f && nan.toward_sun[1] == 0.0f && nan.toward_sun[2] == 0.0f);
        raw.sun_direction[0] = INFINITY;
        const Camera inf = parse_camera(raw);
        check("an infinite sun direction becomes zero", inf.toward_sun[0] == 0.0f && inf.toward_sun[1] == 0.0f && inf.toward_sun[2] == 0.0f);
        raw.sun_color[0] = 4.5f;
        raw.sun_color[1] = 3;
        raw.sun_color[2] = 2.25f;
        raw.sun_color[3] = 9;
        raw.sky_color[0] = 0.2f;
        raw.sky_color[1] = 0.3f;
        raw.sky_color[2] = 0.5f;
        const Camera lit = parse_camera(raw);
        check("the sun color (4.5, 3, 2.25) and sky color (0.2, 0.3, 0.5) are copied", lit.sun_color[0] == 4.5f && lit.sun_color[1] == 3.0f
            && lit.sun_color[2] == 2.25f && lit.sky_color[0] == 0.2f && lit.sky_color[1] == 0.3f && lit.sky_color[2] == 0.5f);
        raw.sun_color[0] = NAN;
        raw.sun_color[1] = -2;
        raw.sky_color[2] = INFINITY;
        const Camera bad = parse_camera(raw);
        check("a NaN, a negative and an infinite color component become 0, and the rest stay", bad.sun_color[0] == 0.0f
            && bad.sun_color[1] == 0.0f && bad.sun_color[2] == 2.25f && bad.sky_color[1] == 0.3f && bad.sky_color[2] == 0.0f);
    }
    {
        check("disabled settings want no step", wanted_steps(view_only(PHOTOREAL_VIEW_HDR_SCENE)).bits != 0
            && wanted_steps(parse_settings(PhotorealSettings { sizeof(PhotorealSettings), 0, PHOTOREAL_VIEW_HDR_SCENE, 1, { 1, 1, 1, 1, 1 }, { 1, 1, 1, 1, 1 }, { 1, 1, 1, 1, 1 }, { 1, 1, 0, 1, 1, 1 }, { 1, 1, 1, 1 } })).empty());
    }
    {
        // Unity's GL.GetGPUProjectionMatrix for a 60 degree, 16:10 camera with near 0.1 and far 1000, reversed Z.
        const float view_to_clip[16] = { 1.0825318f, 0, 0, 0, 0, 1.7320508f, 0, 0, 0, 0, 0.0001f, -1, 0, 0, 0.1f, 0 };
        float clip_to_view[16];
        const bool ok = invert(view_to_clip, clip_to_view);
        // The near plane's center: clip (0, 0, 1, 1) is view (0, 0, -0.1) after the divide by w.
        const float w = clip_to_view[11] + clip_to_view[15];
        const float z = clip_to_view[10] + clip_to_view[14];
        check("the inverse projection maps the reversed-Z near plane to 0.1 in front of the camera", ok && std::fabs(z / w + 0.1f) < 1e-4f);
        const float singular[16] = {};
        check("a singular matrix does not invert", !invert(singular, clip_to_view));
    }
    std::printf("%s: %d failed\n", __FILE__, failures);
    return failures == 0 ? 0 : 1;
}
