// The plan and the report: which passes run at which moment, what the status line says, how settings are parsed.
#include "fixture.h"
#include "../core/plan.h"
#include "../enhancements.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdio>

using namespace enhancements;

namespace
{
    int failures = 0;

    void check(const char *name, bool ok)
    {
        std::printf("%s %s\n", ok ? "PASS" : "FAIL", name);
        failures += ok ? 0 : 1;
    }

    EnhancementsSettings raw_settings()
    {
        EnhancementsSettings raw {};
        raw.size = sizeof raw;
        raw.enabled = 1;
        raw.flip = 1;
        raw.ambient = { 1, 0.4f, 0.8f, 1.5f, 0.7f };
        return raw;
    }

    Settings ambient_on() { return parse_settings(raw_settings()); }

    Settings contact_shadows_on(bool ambient, uint32_t view = ENHANCEMENTS_VIEW_OFF)
    {
        EnhancementsSettings raw = raw_settings();
        raw.ambient.enabled = ambient;
        raw.contact_shadows = { 1, 0.6f, 1, 0.25f, 0 };
        raw.view = view;
        return parse_settings(raw);
    }

    Settings sun_shadows_on(bool contact_shadows)
    {
        EnhancementsSettings raw = raw_settings();
        raw.contact_shadows = { contact_shadows, 0.6f, 1, 0.25f, 0 };
        raw.sun_shadows = { 1, 0.03f, 0.02f, 1 };
        return parse_settings(raw);
    }

    Settings atmosphere_on()
    {
        EnhancementsSettings raw = raw_settings();
        raw.ambient.enabled = 0;
        raw.atmosphere = { 1, 0.000325f, 0.02f, 1, 0.7f };
        return parse_settings(raw);
    }

    Settings tonemap_on(bool atmosphere)
    {
        EnhancementsSettings raw = raw_settings();
        raw.ambient.enabled = 0;
        raw.atmosphere = { atmosphere, 0.000325f, 0.02f, 1, 0.7f };
        raw.tonemap = { 1, 0, ENHANCEMENTS_CURVE_GAME, 1, 1, 1 };
        return parse_settings(raw);
    }

    Settings wetness_on(bool ambient)
    {
        EnhancementsSettings raw = raw_settings();
        raw.ambient.enabled = ambient;
        raw.wetness = { 1, 0, 0.35f, 0.3f };
        return parse_settings(raw);
    }

    Settings light_shafts_on(bool atmosphere)
    {
        EnhancementsSettings raw = raw_settings();
        raw.ambient.enabled = 0;
        raw.atmosphere = { atmosphere, 0.000325f, 0.02f, 1, 0.7f };
        raw.light_shafts = { 1, 1, 0.01f, 10, 0.6f, 150, 0.5f, 6, 1, 48 };
        return parse_settings(raw);
    }

    Settings lens_flare_on()
    {
        EnhancementsSettings raw = raw_settings();
        raw.ambient.enabled = 0;
        raw.lens_flare = { 1, ENHANCEMENTS_FLARE_SOURCE_SUN, { 0.8f, 0.12f }, 1, 1, 1, 1, 1, 1, 0.5f, 1, 0.15f, 1, ENHANCEMENTS_FLARE_PLACE_SCENE, 0.5f };
        return parse_settings(raw);
    }

    // The trace with the three passes it replaces, all on.
    Settings trace_on(uint32_t outputs, uint32_t view = ENHANCEMENTS_VIEW_OFF)
    {
        EnhancementsSettings raw = raw_settings();
        raw.view = view;
        raw.contact_shadows = { 1, 0.6f, 1, 0.25f, 0 };
        raw.sun_shadows = { 1, 0.03f, 0.02f, 1 };
        raw.trace = { 1, outputs, ENHANCEMENTS_TRACE_CAST_CHARACTERS | ENHANCEMENTS_TRACE_CAST_ALPHA_CARDS, 2, 1, 0.0093f, 1, 64, 32, 20 };
        return parse_settings(raw);
    }

    Settings view_only(uint32_t view)
    {
        EnhancementsSettings raw = raw_settings();
        raw.view = view;
        raw.ambient.enabled = 0;
        return parse_settings(raw);
    }

    // A pass the plan ran: the event it ran at and the step it was planned for.
    struct Run
    {
        uint32_t seq;
        Step step;
        PassId pass;
        friend bool operator==(const Run &a, const Run &b) { return a.seq == b.seq && a.step == b.step && a.pass == b.pass; }
    };

    // Replays a fixture as the shell does: at every moment, in kRecipe order, plan the passes, note the plan and the view;
    // finish at the end.
    FrameReport run_frame(std::vector<fixture::Row> &rows, const Settings &settings, const Inputs &inputs, std::vector<Step> *snapshots = nullptr,
        std::vector<Run> *runs = nullptr)
    {
        FrameTracker tracker;
        FrameReport report = FrameReport::start(settings);
        tracker.begin_frame();
        for (fixture::Row &row : rows)
        {
            const StepSet moments = tracker.feed(fixture::event(row));
            for (const StepSpec &spec : kRecipe)
            {
                if (!moments.has(spec.step))
                    continue;
                const Plan p = plan(spec.step, tracker.map(), settings, inputs);
                report.note(p);
                for (uint8_t i = 0; i < p.count && runs; i++)
                    if (p.items[i].kind == Decision::Kind::run)
                        runs->push_back({ row.seq, spec.step, p.items[i].pass });
                if (report.note_view(spec.step, tracker.map()) && snapshots)
                    snapshots->push_back(spec.step);
            }
        }
        report.finish(tracker, settings, inputs);
        return report;
    }

    // A camera, and sun constants that passed their checks this frame.
    const Inputs kCamera { true, SunCheck::ok };
    const Inputs kNoCamera { false, SunCheck::ok };

    const char *kAllSteps = "1152x720 found gbuffer quarter-shadow shadow-mask ambient-pair combine bloom tonemap forward gbuffer-done tonemap-read";
}

int main()
{
    const std::vector<fixture::Row> walk = fixture::load("fixtures/capture-20261009-214859.tsv");
    {
        auto rows = walk;
        const FrameReport report = run_frame(rows, ambient_on(), kCamera);
        check("ambient runs at combine", report.ran.bits == ENHANCEMENTS_PASS_AMBIENT && report.skipped.empty());
        check("describe names every step and the pass", describe(report) == std::string(kAllSteps) + "; ran ambient@combine");
        check("steps wanted by ambient: gbuffer and combine", report.wanted.bits == (ENHANCEMENTS_STEP_GBUFFER | ENHANCEMENTS_STEP_COMBINE));
    }
    {
        auto rows = walk;
        const FrameReport report = run_frame(rows, contact_shadows_on(true), kCamera);
        check("contact shadows and ambient both run", report.ran.bits == (ENHANCEMENTS_PASS_CONTACT_SHADOWS | ENHANCEMENTS_PASS_AMBIENT)
            && report.skipped.empty());
        check("describe lists contact shadows before ambient at combine",
            describe(report) == std::string(kAllSteps) + "; ran contact-shadows@combine ambient@combine");
        check("steps wanted by both: gbuffer and combine", report.wanted.bits == (ENHANCEMENTS_STEP_GBUFFER | ENHANCEMENTS_STEP_COMBINE));
    }
    {
        auto rows = walk;
        FrameTracker tracker;
        fixture::replay(rows, tracker);
        const Plan p = plan(Step::combine, tracker.map(), contact_shadows_on(true), kCamera);
        check("the combine plan holds the trace and sun shadows, off, then runs contact shadows, then ambient", p.count == 4
            && p.items[0].pass == PassId::trace && p.items[0].kind == Decision::Kind::off
            && p.items[1].pass == PassId::sun_shadows && p.items[1].kind == Decision::Kind::off
            && p.items[2].pass == PassId::contact_shadows && p.items[2].kind == Decision::Kind::run
            && p.items[3].pass == PassId::ambient && p.items[3].kind == Decision::Kind::run);
        const Plan at_tonemap = plan(Step::tonemap, tracker.map(), contact_shadows_on(true), kCamera);
        check("the shadow-mask step has no pass, and the tonemap step only tonemap, off", plan(Step::shadow_mask, tracker.map(), contact_shadows_on(true), kCamera).count == 0
            && at_tonemap.count == 1 && at_tonemap.items[0].pass == PassId::tonemap && at_tonemap.items[0].kind == Decision::Kind::off);
    }
    {
        auto rows = walk;
        const FrameReport report = run_frame(rows, sun_shadows_on(true), kCamera);
        check("sun shadows run at combine before contact shadows and ambient", report.ran.bits
            == (ENHANCEMENTS_PASS_SUN_SHADOWS | ENHANCEMENTS_PASS_CONTACT_SHADOWS | ENHANCEMENTS_PASS_AMBIENT) && describe(report)
            == std::string(kAllSteps) + "; ran sun-shadows@combine contact-shadows@combine ambient@combine");
        check("steps wanted by sun shadows: gbuffer and combine", report.wanted.bits == (ENHANCEMENTS_STEP_GBUFFER | ENHANCEMENTS_STEP_COMBINE));
        FrameTracker tracker;
        fixture::replay(rows, tracker);
        const Plan p = plan(Step::combine, tracker.map(), sun_shadows_on(true), kCamera);
        check("the combine plan runs sun shadows first after the trace", p.count == 4 && p.items[0].pass == PassId::trace
            && p.items[1].pass == PassId::sun_shadows && p.items[1].kind == Decision::Kind::run && p.items[2].pass == PassId::contact_shadows);
    }
    {
        auto rows = walk;
        const FrameReport report = run_frame(rows, sun_shadows_on(false), Inputs { true, SunCheck::unread });
        check("before any constants were read back, sun shadows skip and say so, and ambient runs", report.skipped.bits
            == ENHANCEMENTS_PASS_SUN_SHADOWS && report.ran.bits == ENHANCEMENTS_PASS_AMBIENT && report.missing.empty() && describe(report)
            == std::string(kAllSteps) + "; ran ambient@combine; skipped sun-shadows: sun constants unread");
    }
    {
        auto rows = walk;
        const FrameReport report = run_frame(rows, sun_shadows_on(true), Inputs { false, SunCheck::radius });
        check("constants that failed a check and no camera: both reasons", describe(report) == std::string(kAllSteps)
            + "; skipped sun-shadows: no camera, sun constants radius; skipped contact-shadows: no camera; skipped ambient: no camera");
    }
    {
        auto rows = walk;
        fixture::find(rows, 893).draw.inputs.resize(2);
        const FrameReport report = run_frame(rows, sun_shadows_on(false), Inputs { true, SunCheck::unread });
        check("a shadow-mask draw without the atlas at t2: sun shadows miss sun-atlas", report.missing.bits == ENHANCEMENTS_ENTRY_SUN_ATLAS
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
            report.wanted.bits == (ENHANCEMENTS_STEP_GBUFFER | ENHANCEMENTS_STEP_COMBINE | ENHANCEMENTS_STEP_BLOOM));
        FrameTracker tracker;
        fixture::replay(rows, tracker);
        const Plan at_bloom = plan(Step::bloom, tracker.map(), atmosphere_on(), kCamera);
        check("the bloom plan runs atmosphere and holds light shafts, the depth of field and the lens flare, off", at_bloom.count == 4
            && at_bloom.items[0].pass == PassId::atmosphere && at_bloom.items[0].kind == Decision::Kind::run && at_bloom.items[1].pass == PassId::light_shafts
            && at_bloom.items[1].kind == Decision::Kind::off && at_bloom.items[2].pass == PassId::depth_of_field && at_bloom.items[2].kind == Decision::Kind::off
            && at_bloom.items[3].pass == PassId::lens_flare && at_bloom.items[3].kind == Decision::Kind::off);
        check("the combine plan holds the trace, sun shadows, contact shadows and ambient, all off",
            plan(Step::combine, tracker.map(), atmosphere_on(), kCamera).count == 4
            && plan(Step::combine, tracker.map(), atmosphere_on(), kCamera).items[0].kind == Decision::Kind::off);
    }
    {
        auto rows = walk;
        const FrameReport report = run_frame(rows, atmosphere_on(), kNoCamera);
        check("without a camera, atmosphere skips and says so", report.skipped.bits == ENHANCEMENTS_PASS_ATMOSPHERE
            && describe(report) == std::string(kAllSteps) + "; skipped atmosphere: no camera");
    }
    {
        auto rows = walk;
        fixture::drop(rows, 1088, 1088);  // the first bloom draw, the one that samples the HDR scene
        const FrameReport report = run_frame(rows, atmosphere_on(), kCamera);
        check("no bloom draw: atmosphere runs at the tonemap draw instead, and the line says so", describe(report)
            == "1152x720 found gbuffer quarter-shadow shadow-mask ambient-pair combine tonemap forward gbuffer-done tonemap-read; ran atmosphere@tonemap");
    }
    {
        auto rows = walk;
        fixture::drop(rows, 1088, 1200);  // the first bloom draw, and everything from the tonemap on
        const FrameReport report = run_frame(rows, atmosphere_on(), kCamera);
        check("no bloom draw and no tonemap draw: atmosphere skips and names the bloom step", describe(report)
            == "1152x720 found gbuffer quarter-shadow shadow-mask ambient-pair combine forward gbuffer-done; skipped atmosphere: no bloom");
    }
    {
        std::vector<fixture::Row> menu;
        const FrameReport report = run_frame(menu, atmosphere_on(), kCamera);
        check("no G-buffer: atmosphere names the entries it needs", describe(report) == "found nothing; skipped atmosphere: missing depth hdr-scene"
            && report.missing.bits == (ENHANCEMENTS_ENTRY_DEPTH | ENHANCEMENTS_ENTRY_HDR_SCENE));
    }
    {
        auto rows = walk;
        const FrameReport report = run_frame(rows, tonemap_on(true), kCamera);
        check("atmosphere runs at bloom and tonemap at tonemap", describe(report) == std::string(kAllSteps) + "; ran atmosphere@bloom tonemap@tonemap");
        check("steps wanted by atmosphere and tonemap: gbuffer, combine, bloom and tonemap",
            report.wanted.bits == (ENHANCEMENTS_STEP_GBUFFER | ENHANCEMENTS_STEP_COMBINE | ENHANCEMENTS_STEP_BLOOM | ENHANCEMENTS_STEP_TONEMAP));
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
        check("without a final bloom, tonemap still runs", report.ran.bits == ENHANCEMENTS_PASS_TONEMAP && report.missing.empty());
    }
    {
        auto rows = walk;
        fixture::drop(rows, 1100, 1200);  // everything from the tonemap on
        const FrameReport report = run_frame(rows, tonemap_on(false), kCamera);
        check("no tonemap draw: tonemap skips for want of the output it draws into", describe(report)
            == "1152x720 found gbuffer quarter-shadow shadow-mask ambient-pair combine bloom forward gbuffer-done; skipped tonemap: missing tonemap-out");
    }
    {
        std::vector<fixture::Row> menu;
        const FrameReport report = run_frame(menu, tonemap_on(true), kCamera);
        check("no G-buffer: atmosphere and tonemap name the entries they need", describe(report)
            == "found nothing; skipped atmosphere: missing depth hdr-scene; skipped tonemap: missing hdr-scene tonemap-out");
    }
    {
        auto rows = walk;
        const FrameReport report = run_frame(rows, light_shafts_on(true), kCamera);
        check("atmosphere, then light shafts run at bloom", report.ran.bits == (ENHANCEMENTS_PASS_ATMOSPHERE | ENHANCEMENTS_PASS_LIGHT_SHAFTS)
            && describe(report) == std::string(kAllSteps) + "; ran atmosphere@bloom light-shafts@bloom");
        check("steps wanted by light shafts: gbuffer, combine and bloom",
            report.wanted.bits == (ENHANCEMENTS_STEP_GBUFFER | ENHANCEMENTS_STEP_COMBINE | ENHANCEMENTS_STEP_BLOOM));
        FrameTracker tracker;
        fixture::replay(rows, tracker);
        const Plan at_bloom = plan(Step::bloom, tracker.map(), light_shafts_on(true), kCamera);
        check("the bloom plan runs atmosphere first, then light shafts with this frame's sun constants", at_bloom.count == 4
            && at_bloom.items[0].pass == PassId::atmosphere && at_bloom.items[0].kind == Decision::Kind::run
            && at_bloom.items[1].pass == PassId::light_shafts && at_bloom.items[1].kind == Decision::Kind::run && at_bloom.items[1].uses_sun);
    }
    {
        auto rows = walk;
        const FrameReport report = run_frame(rows, lens_flare_on(), kCamera);
        check("the lens flare's visibility runs at bloom", report.ran.bits == ENHANCEMENTS_PASS_LENS_FLARE
            && describe(report) == std::string(kAllSteps) + "; ran lens-flare@bloom");
        FrameTracker tracker;
        fixture::replay(rows, tracker);
        const Plan read = plan(Step::bloom, tracker.map(), lens_flare_on(), kCamera);
        check("with this frame's sun constants it reads the atlas", read.items[3].pass == PassId::lens_flare && read.items[3].kind == Decision::Kind::run
            && read.items[3].uses_sun);
        const Plan unread = plan(Step::bloom, tracker.map(), lens_flare_on(), Inputs { true, SunCheck::unread });
        check("without them it still runs, from the depth alone", unread.items[3].kind == Decision::Kind::run && !unread.items[3].uses_sun);
        check("the lens flare asks for the sun constants", wants_sun(lens_flare_on()));
    }
    {
        auto rows = walk;
        const FrameReport report = run_frame(rows, light_shafts_on(true), Inputs { true, SunCheck::unread });
        check("before any sun constants were read back, light shafts skip and say so, and atmosphere runs", report.ran.bits == ENHANCEMENTS_PASS_ATMOSPHERE
            && report.skipped.bits == ENHANCEMENTS_PASS_LIGHT_SHAFTS && report.missing.empty()
            && describe(report) == std::string(kAllSteps) + "; ran atmosphere@bloom; skipped light-shafts: sun constants unread");
    }
    {
        auto rows = walk;
        check("without a camera, light shafts skip and say so", describe(run_frame(rows, light_shafts_on(false), kNoCamera))
            == std::string(kAllSteps) + "; skipped light-shafts: no camera");
    }
    {
        auto rows = walk;
        fixture::find(rows, 893).draw.inputs.resize(2);
        const FrameReport report = run_frame(rows, light_shafts_on(false), Inputs { true, SunCheck::unread });
        check("a shadow-mask draw without the atlas at t2: light shafts miss sun-atlas", report.missing.bits == ENHANCEMENTS_ENTRY_SUN_ATLAS
            && describe(report) == std::string(kAllSteps) + "; skipped light-shafts: missing sun-atlas, sun constants unread");
    }
    {
        auto rows = walk;
        fixture::drop(rows, 1088, 1088);  // the first bloom draw, the one that samples the HDR scene
        check("no bloom draw: light shafts run at the tonemap draw instead", describe(run_frame(rows, light_shafts_on(false), kCamera))
            == "1152x720 found gbuffer quarter-shadow shadow-mask ambient-pair combine tonemap forward gbuffer-done tonemap-read; ran light-shafts@tonemap");
    }
    {
        // Build 196's flicker: frames where the game's bloom does not match, but its tonemap draw does.
        auto rows = fixture::load("fixtures/capture-20261010-111824.tsv");
        std::vector<Run> normal_runs;
        const FrameReport normal = run_frame(rows, tonemap_on(true), kCamera, nullptr, &normal_runs);
        check("111824 as captured: atmosphere at the bloom draw 921, tonemap at the tonemap draw 951", normal_runs == std::vector<Run> {
            { 921, Step::bloom, PassId::atmosphere }, { 951, Step::tonemap, PassId::tonemap } } && normal.fell_back.empty());
        Settings settings = light_shafts_on(true);
        settings.tonemap.enabled = true;
        auto missing = rows;
        fixture::drop(missing, 921, 921);
        std::vector<Run> runs;
        const FrameReport report = run_frame(missing, settings, kCamera, nullptr, &runs);
        check("111824 without the bloom draw 921: atmosphere and light shafts are planned once, at the tonemap draw 951, before tonemap",
            runs == std::vector<Run> { { 951, Step::bloom, PassId::atmosphere }, { 951, Step::bloom, PassId::light_shafts },
                { 951, Step::tonemap, PassId::tonemap } });
        check("and the line names the tonemap step as their moment, with bloom not found", describe(report)
            == "1920x1200 found gbuffer quarter-shadow shadow-mask ambient-pair combine tonemap forward gbuffer-done tonemap-read; ran atmosphere@tonemap "
               "light-shafts@tonemap tonemap@tonemap" && report.fell_back.bits == ENHANCEMENTS_STEP_BLOOM && report.skipped.empty());
        auto other = rows;
        fixture::find(other, 919).targets.color[0].size = { 960, 600 };  // a half-size first bloom target
        runs.clear();
        run_frame(other, settings, kCamera, nullptr, &runs);
        check("111824 with the first bloom target at half size: the same three runs at 951", runs == std::vector<Run> {
            { 951, Step::bloom, PassId::atmosphere }, { 951, Step::bloom, PassId::light_shafts }, { 951, Step::tonemap, PassId::tonemap } });
        auto late = missing;
        fixture::insert_before(late, 952, fixture::slice(rows, 919, 921));
        runs.clear();
        run_frame(late, settings, kCamera, nullptr, &runs);
        check("a bloom draw after the tonemap draw does not run the bloom passes a second time", runs == std::vector<Run> {
            { 951, Step::bloom, PassId::atmosphere }, { 951, Step::bloom, PassId::light_shafts }, { 951, Step::tonemap, PassId::tonemap } });
        std::vector<Step> snapshots;
        Settings view = light_shafts_on(false);
        view.view = View::light_shafts;
        const FrameReport shown = run_frame(missing, view, kCamera, &snapshots);
        check("the light-shafts view snapshots at the bloom step's moment, at the tonemap draw, and is shown",
            snapshots == std::vector<Step> { Step::bloom } && shown.view_shown() == View::light_shafts && describe(shown)
                == "1920x1200 found gbuffer quarter-shadow shadow-mask ambient-pair combine tonemap forward gbuffer-done tonemap-read; ran light-shafts@tonemap; view light-shafts");
    }
    {
        std::vector<fixture::Row> menu;
        check("no G-buffer: light shafts name the entries they need", describe(run_frame(menu, light_shafts_on(false), Inputs { true, SunCheck::unread }))
            == "found nothing; skipped light-shafts: missing depth hdr-scene sun-atlas, sun constants unread");
    }
    {
        auto rows = fixture::load("fixtures/capture-20261010-111824.tsv");
        check("111824, the user's normal settings: atmosphere, then light shafts at bloom", describe(run_frame(rows, light_shafts_on(true), kCamera))
            == "1920x1200 found gbuffer quarter-shadow shadow-mask ambient-pair combine bloom tonemap forward gbuffer-done tonemap-read; ran atmosphere@bloom light-shafts@bloom");
    }
    {
        const std::vector<uint8_t> lattice = noise_lattice();
        std::array<uint32_t, 256> counts {};
        double sum = 0;
        for (const uint8_t value : lattice)
        {
            counts[value]++;
            sum += value;
        }
        check("the noise lattice holds 64^3 bytes, the same ones every time", lattice.size() == 262144 && noise_lattice() == lattice);
        check("its bytes take every value from 0 to 255 and average 127.5 within 1", std::fabs(sum / lattice.size() - 127.5) < 1
            && std::find(counts.begin(), counts.end(), 0u) == counts.end());
        uint32_t repeats = 0;
        for (size_t i = 1; i < lattice.size(); i++)
            repeats += lattice[i] == lattice[i - 1];
        check("neighbors are unrelated: about 1 in 256 repeat the byte before them", repeats > 700 && repeats < 1350);
    }
    {
        auto rows = walk;
        FrameTracker tracker;
        fixture::replay(rows, tracker);
        const Plan at_combine = plan(Step::combine, tracker.map(), sun_shadows_on(false), kCamera);
        check("with this frame's sun constants, sun shadows use them", at_combine.items[1].pass == PassId::sun_shadows
            && at_combine.items[1].kind == Decision::Kind::run && at_combine.items[1].uses_sun);
        check("ambient never uses the sun constants", at_combine.items[3].pass == PassId::ambient && at_combine.items[3].kind == Decision::Kind::run
            && !at_combine.items[3].uses_sun);
    }
    {
        check("sun shadows and light shafts want the sun constants", wants_sun(sun_shadows_on(false)) && wants_sun(light_shafts_on(false)));
        check("ambient, contact shadows, atmosphere, tonemap and wetness do not", !wants_sun(ambient_on()) && !wants_sun(contact_shadows_on(true))
            && !wants_sun(atmosphere_on()) && !wants_sun(tonemap_on(true)) && !wants_sun(wetness_on(true)));
        Settings off = sun_shadows_on(false);
        off.enabled = false;
        check("disabled settings want no sun constants", !wants_sun(off));
    }
    {
        auto rows = walk;
        const FrameReport report = run_frame(rows, wetness_on(false), kCamera);
        check("wetness runs at gbuffer-done", report.ran.bits == ENHANCEMENTS_PASS_WETNESS
            && describe(report) == std::string(kAllSteps) + "; ran wetness@gbuffer-done");
        check("steps wanted by wetness: gbuffer and gbuffer-done", report.wanted.bits == (ENHANCEMENTS_STEP_GBUFFER | ENHANCEMENTS_STEP_GBUFFER_DONE));
        FrameTracker tracker;
        fixture::replay(rows, tracker);
        const Plan at_done = plan(Step::gbuffer_done, tracker.map(), wetness_on(false), kCamera);
        check("the gbuffer-done plan runs wetness alone", at_done.count == 1 && at_done.items[0].pass == PassId::wetness
            && at_done.items[0].kind == Decision::Kind::run);
    }
    {
        auto rows = fixture::load("fixtures/capture-20261010-111824.tsv");
        const FrameReport report = run_frame(rows, wetness_on(true), kCamera);
        check("111824, the user's normal settings: wetness at gbuffer-done, then ambient at combine", describe(report)
            == "1920x1200 found gbuffer quarter-shadow shadow-mask ambient-pair combine bloom tonemap forward gbuffer-done tonemap-read; ran wetness@gbuffer-done ambient@combine");
    }
    {
        auto rows = walk;
        const FrameReport report = run_frame(rows, wetness_on(false), kNoCamera);
        check("without a camera, wetness skips and says so", report.skipped.bits == ENHANCEMENTS_PASS_WETNESS && report.ran.empty()
            && describe(report) == std::string(kAllSteps) + "; skipped wetness: no camera");
    }
    {
        auto rows = fixture::slice(walk, 0, 679);  // the frame up to the half-size pass's draw
        const FrameReport report = run_frame(rows, wetness_on(false), kCamera);
        check("a frame that ends before gbuffer-done: wetness skips and names the step", describe(report)
            == "1152x720 found gbuffer; skipped wetness: no gbuffer-done");
    }
    {
        std::vector<fixture::Row> menu;
        const FrameReport report = run_frame(menu, wetness_on(false), kCamera);
        check("no G-buffer: wetness names the entries it needs", describe(report)
            == "found nothing; skipped wetness: missing normals albedo specular material-id smoothness depth");
    }
    {
        const auto near = [](float a, float b) { return std::fabs(a - b) < 1e-5f; };
        check("g 0 scatters evenly: 1 / (4 pi) toward the sun and away from it", near(henyey_greenstein(1, 0), 0.0795775f)
            && near(henyey_greenstein(-1, 0), 0.0795775f));
        check("g 0.6: 0.795775 toward the sun, 0.0124340 away from it", near(henyey_greenstein(1, 0.6f), 0.795775f)
            && near(henyey_greenstein(-1, 0.6f), 0.0124340f));
        double total = 0;
        for (int i = 0; i < 20000; i++)
            total += 2 * 3.14159265 * henyey_greenstein(-1 + (i + 0.5f) / 10000, 0.6f) / 10000;
        check("g 0.6 integrates to 1 over the sphere", std::fabs(total - 1) < 1e-3);
    }
    {
        const auto near = [](float a, float b) { return std::fabs(a - b) <= 1e-5f * std::fabs(b); };
        check("at the camera's height, even mist has the density", mist_density(0.01f, 0, 10, 0.3f, 0) == 0.01f);
        check("10 m above the camera at height 10 it is 1/e as dense, 10 m below e times", near(mist_density(0.01f, 10, 10, 0.5f, 0), 0.00367879f)
            && near(mist_density(0.01f, -10, 10, 0.5f, 0), 0.0271828f));
        check("far below the camera it stops at e^3 times as dense", near(mist_density(0.01f, -1000, 10, 0.5f, 0), 0.200855f)
            && near(mist_density(0.01f, -30, 10, 0.5f, 0), 0.200855f));
        check("far above the camera it is clear", mist_density(0.01f, 5000, 10, 0.5f, 0) < 1e-30f);
        check("patchiness 1: noise 0.25 and below is a hole, 0.75 and above twice as dense, 0.5 the density",
            mist_density(0.01f, 0, 10, 0.2f, 1) == 0.0f && near(mist_density(0.01f, 0, 10, 0.8f, 1), 0.02f)
                && near(mist_density(0.01f, 0, 10, 0.5f, 1), 0.01f));
        check("patchiness 0.5 halves the noise's reach: a hole is half as dense, a bank 1.5 times",
            near(mist_density(0.01f, 0, 10, 0, 0.5f), 0.005f) && near(mist_density(0.01f, 0, 10, 1, 0.5f), 0.015f));
        check("the patch factor is symmetric about noise 0.5, so it averages 1",
            near(mist_density(1, 0, 10, 0.4f, 1) + mist_density(1, 0, 10, 0.6f, 1), 2));
    }
    {
        double spans = 0;
        for (int i = 0; i < 48; i++)
            spans += march_span(static_cast<float>(i), 48, 150);
        check("48 spans cover a 150 m ray", std::fabs(spans - 150) < 1e-3);
        check("sample 0 of 48 over 150 m runs from 0 to 0.0651 m, and the last from 143.8 m to 150 m", march_at(0, 48, 0, 150) == 0.0f
            && std::fabs(march_at(0, 48, 1, 150) - 0.0651042f) < 1e-6f && std::fabs(march_at(47, 48, 0, 150) - 143.815f) < 1e-3f
            && march_at(47, 48, 1, 150) == 150.0f);
        check("a sample's span is the distance between its start and the next one's",
            std::fabs(march_span(10, 48, 150) - (march_at(11, 48, 0, 150) - march_at(10, 48, 0, 150))) < 1e-4f);
    }
    {
        const auto near = [](float a, float b) { return std::fabs(a - b) < 1e-5f; };
        const auto march = [](int steps, float density, float meters, float light) {
            Mist m;
            for (int i = 0; i < steps; i++)
                m = mist_step(m, density, meters / steps, light);
            return m;
        };
        const Mist one = march(1, 0.01f, 100, 2), many = march(48, 0.01f, 100, 2);
        check("100 m of even mist at 0.01 lets exp(-1) through, in one step or 48", near(one.transmittance, 0.367879f)
            && near(many.transmittance, 0.367879f));
        check("and scatters light 2 times (1 - exp(-1)), in one step or 48", near(one.inscatter, 1.26424f) && near(many.inscatter, 1.26424f));
        check("over a scene of 0.5 it shows 0.5 exp(-1) + 1.26424", near(mist_over(0.5f, many), 1.44818f));
        const Mist shadowed = march(48, 0.01f, 100, mist_light(5, 0, 0.4f));
        check("a march the sun never reaches scatters only the sky's 0.4 (1 - exp(-1))", near(shadowed.inscatter, 0.252848f));
        check("half in the sun: sun 5 and sky 0.4 scatter 2.9 per unit", mist_light(5, 0.5f, 0.4f) == 2.9f);
        const Mist clear = march(48, 0, 100, 3);
        check("no mist leaves the scene as it was", clear.transmittance == 1.0f && clear.inscatter == 0.0f && mist_over(0.7f, clear) == 0.7f);
        const Mist short_ray = march(48, 0.01f, 50, 2), long_ray = march(48, 0.01f, 150, 2), taken = mist_rescale(short_ray, 3);
        check("50 m of even mist taken to 150 m is 150 m of it", near(taken.transmittance, long_ray.transmittance)
            && near(taken.inscatter, long_ray.inscatter));
        const Mist back = mist_rescale(long_ray, 1.0f / 3);
        check("and 150 m taken to 50 m is 50 m of it", near(back.transmittance, short_ray.transmittance) && near(back.inscatter, short_ray.inscatter));
        const Mist thin = march(1, 1e-7f, 100, 2);
        check("a march with almost no mist doubles its light when taken twice as far", mist_rescale(thin, 2).inscatter == 2 * thin.inscatter);
        check("a march with no mist stays clear at any length", mist_rescale(clear, 5).transmittance == 1.0f && mist_rescale(clear, 5).inscatter == 0.0f);
    }
    {
        Camera rain {};
        rain.wetness = 0.2f;
        WetnessSettings settings;
        check("wetness 0 follows the weather's 0.2", wetness_amount(settings, rain) == 0.2f);
        settings.wetness = 0.75f;
        check("wetness 0.75 replaces the weather's 0.2", wetness_amount(settings, rain) == 0.75f);
        rain.wetness = 1;
        settings.wetness = 0.1f;
        check("wetness 0.1 replaces a soaked weather too", wetness_amount(settings, rain) == 0.1f);
    }
    {
        const auto near = [](float a, float b) { return std::fabs(a - b) < 1e-5f; };
        check("a soaked ground pixel is fully wet, a wall half, an overhang half", wet_share(1, 1, 0) == 1.0f && wet_share(1, 0, 0) == 0.5f
            && wet_share(1, -1, 0) == 0.5f);
        check("at wetness 0.4, flat ground is 0.4 wet and a 60-degree slope 0.3", near(wet_share(0.4f, 1, 0), 0.4f) && near(wet_share(0.4f, 0.5f, 0), 0.3f));
        check("a puddle makes a pixel fully wet at any wetness", wet_share(0.2f, 1, 1) == 1.0f && near(wet_share(0.2f, 1, 0.5f), 0.6f));
        check("dry, nothing is wet", wet_share(0, 1, 0) == 0.0f);
        check("puddles 0.3 when soaked: noise above 0.75 is puddle, below 0.7 is not, between is the soft edge",
            puddle_cover(0.8f, 1, 1, 0.3f) == 1.0f && puddle_cover(0.69f, 1, 1, 0.3f) == 0.0f && near(puddle_cover(0.725f, 1, 1, 0.3f), 0.5f));
        check("puddles grow with wetness: at wetness 0.5 the threshold is 0.85", puddle_cover(0.8f, 1, 0.5f, 0.3f) == 0.0f
            && puddle_cover(0.91f, 1, 0.5f, 0.3f) == 1.0f);
        check("dry ground and puddles 0 have no puddles, even at noise 1", puddle_cover(1, 1, 0, 0.3f) == 0.0f && puddle_cover(1, 1, 1, 0) == 0.0f);
        check("ground steeper than up 0.95 has no puddles, and they fade in up to 0.98", puddle_cover(0.9f, 0.95f, 1, 0.3f) == 0.0f
            && near(puddle_cover(0.9f, 0.965f, 1, 0.3f), 0.5f) && puddle_cover(0.9f, 0.98f, 1, 0.3f) == 1.0f);
        check("smoothness 0.3 wets to 0.9, halfway to 0.6, and in a puddle to 0.97", near(wet_smoothness(0.3f, 1, 0), 0.9f)
            && near(wet_smoothness(0.3f, 0.5f, 0), 0.6f) && near(wet_smoothness(0.3f, 1, 1), 0.97f));
        check("dry smoothness stays as stored", wet_smoothness(0.3f, 0, 0) == 0.3f);
    }
    {
        auto rows = fixture::load("fixtures/capture-20261010-111824.tsv");
        const FrameReport report = run_frame(rows, contact_shadows_on(true), kCamera);
        check("111824, the user's normal settings: contact shadows and ambient both run", describe(report)
            == "1920x1200 found gbuffer quarter-shadow shadow-mask ambient-pair combine bloom tonemap forward gbuffer-done tonemap-read; ran contact-shadows@combine ambient@combine");
    }
    {
        auto rows = walk;
        fixture::drop(rows, 892, 892);  // the shadow mask's clear, so no shadow mask is found
        const FrameReport report = run_frame(rows, contact_shadows_on(true), kCamera);
        check("without the shadow mask, contact shadows skip and ambient runs",
            report.skipped.bits == ENHANCEMENTS_PASS_CONTACT_SHADOWS && report.ran.bits == ENHANCEMENTS_PASS_AMBIENT);
        check("and the shadow mask is the missing entry", report.missing.bits == ENHANCEMENTS_ENTRY_SHADOW_MASK);
        check("describe says why", describe(report)
            == "1152x720 found gbuffer quarter-shadow ambient-pair combine bloom tonemap forward gbuffer-done tonemap-read; ran ambient@combine; skipped contact-shadows: missing shadow-mask");
    }
    {
        auto rows = walk;
        const FrameReport report = run_frame(rows, contact_shadows_on(true), kNoCamera);
        check("without a camera, both passes skip and say so", report.skipped.bits == (ENHANCEMENTS_PASS_CONTACT_SHADOWS | ENHANCEMENTS_PASS_AMBIENT)
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
        const FrameReport report = run_frame(rows, contact_shadows_on(false, ENHANCEMENTS_VIEW_SHADOW_MASK), kCamera, &snapshots);
        check("the shadow-mask view snapshots at combine, the moment contact shadows run",
            snapshots == std::vector<Step> { Step::combine } && report.ran.bits == ENHANCEMENTS_PASS_CONTACT_SHADOWS);
        check("describe names the pass and the view", describe(report) == std::string(kAllSteps) + "; ran contact-shadows@combine; view shadow-mask");
    }
    {
        auto rows = walk;
        fixture::drop(rows, 910, 913);
        const FrameReport report = run_frame(rows, ambient_on(), kCamera);
        check("without the ambient pair, ambient skips", report.skipped.bits == ENHANCEMENTS_PASS_AMBIENT && report.ran.empty());
        check("and reports the missing entry", report.missing.bits == ENHANCEMENTS_ENTRY_AMBIENT_DIFFUSE);
        check("describe says why",
            describe(report) == "1152x720 found gbuffer quarter-shadow shadow-mask combine bloom tonemap forward gbuffer-done tonemap-read; skipped ambient: missing ambient-diffuse");
    }
    {
        auto rows = walk;
        const FrameReport report = run_frame(rows, ambient_on(), kNoCamera);
        check("without a camera, ambient skips and says so", report.skipped.bits == ENHANCEMENTS_PASS_AMBIENT
            && describe(report) == std::string(kAllSteps) + "; skipped ambient: no camera");
    }
    {
        std::vector<fixture::Row> menu;  // a frame with no G-buffer: a loading screen or a menu
        const FrameReport report = run_frame(menu, ambient_on(), kCamera);
        check("no G-buffer: nothing runs", report.ran.empty() && report.skipped.bits == ENHANCEMENTS_PASS_AMBIENT);
        check("no G-buffer: describe says so", describe(report) == "found nothing; skipped ambient: missing normals depth ambient-diffuse");
        check("no G-buffer: status bits name the missing entries",
            report.missing.bits == (ENHANCEMENTS_ENTRY_NORMALS | ENHANCEMENTS_ENTRY_DEPTH | ENHANCEMENTS_ENTRY_AMBIENT_DIFFUSE));
    }
    {
        auto rows = walk;
        fixture::drop(rows, 924, 926);  // the HDR scene's bind and clear: every entry ambient needs, but no combine moment
        const FrameReport report = run_frame(rows, ambient_on(), kCamera);
        check("no combine moment: ambient skips and names the step",
            describe(report) == "1152x720 found gbuffer quarter-shadow shadow-mask ambient-pair gbuffer-done; skipped ambient: no combine");
    }
    {
        auto rows = walk;
        std::vector<Step> snapshots;
        const FrameReport report = run_frame(rows, view_only(ENHANCEMENTS_VIEW_NORMALS), kCamera, &snapshots);
        check("the normals view snapshots once, at combine", snapshots == std::vector<Step> { Step::combine });
        check("the normals view is shown", report.view_shown() == View::normals);
        check("describe names the view", describe(report) == std::string(kAllSteps) + "; view normals");
    }
    {
        auto rows = walk;
        std::vector<Step> snapshots;
        const FrameReport report = run_frame(rows, view_only(ENHANCEMENTS_VIEW_HDR_SCENE), kCamera, &snapshots);
        check("the HDR view snapshots at tonemap", snapshots == std::vector<Step> { Step::tonemap } && report.view_shown() == View::hdr_scene);
        check("the HDR view wants the steps up to tonemap",
            report.wanted.bits == (ENHANCEMENTS_STEP_GBUFFER | ENHANCEMENTS_STEP_COMBINE | ENHANCEMENTS_STEP_TONEMAP));
    }
    {
        auto rows = fixture::load("fixtures/capture-20261010-111824.tsv");
        std::vector<Step> snapshots;
        const FrameReport report = run_frame(rows, view_only(ENHANCEMENTS_VIEW_BLOOM_FINAL), kCamera, &snapshots);
        check("the bloom-final view snapshots at tonemap and is shown",
            snapshots == std::vector<Step> { Step::tonemap } && report.view_shown() == View::bloom_final);
        check("describe names the bloom-final view", describe(report)
            == "1920x1200 found gbuffer quarter-shadow shadow-mask ambient-pair combine bloom tonemap forward gbuffer-done tonemap-read; view bloom-final");
    }
    {
        auto rows = fixture::load("fixtures/capture-20261010-111824.tsv");
        std::vector<Step> snapshots;
        const FrameReport report = run_frame(rows, view_only(ENHANCEMENTS_VIEW_SUN_ATLAS), kCamera, &snapshots);
        check("the sun-atlas view snapshots at the shadow-mask draw and is shown",
            snapshots == std::vector<Step> { Step::shadow_mask } && report.view_shown() == View::sun_atlas);
        check("the sun-atlas view wants the gbuffer and shadow-mask steps",
            report.wanted.bits == (ENHANCEMENTS_STEP_GBUFFER | ENHANCEMENTS_STEP_SHADOW_MASK));
        check("describe names the sun-atlas view", describe(report)
            == "1920x1200 found gbuffer quarter-shadow shadow-mask ambient-pair combine bloom tonemap forward gbuffer-done tonemap-read; view sun-atlas");
    }
    {
        auto rows = walk;
        std::vector<Step> snapshots;
        Settings settings = light_shafts_on(false);
        settings.view = View::light_shafts;
        const FrameReport report = run_frame(rows, settings, kCamera, &snapshots);
        check("the light-shafts view snapshots at bloom, after the pass that draws it, and is shown",
            snapshots == std::vector<Step> { Step::bloom } && report.view_shown() == View::light_shafts && report.missing.empty());
        check("describe names the pass and the light-shafts view", describe(report) == std::string(kAllSteps) + "; ran light-shafts@bloom; view light-shafts");
        const Label label = view_label(View::light_shafts, true);
        check("the light-shafts label spells LIGHT-SHAFTS", std::vector<int>(label.glyphs.begin(), label.glyphs.begin() + label.length)
            == std::vector<int> { 12, 9, 7, 8, 20, 37, 19, 8, 1, 6, 20, 19 });
    }
    {
        auto rows = walk;
        const FrameReport report = run_frame(rows, view_only(ENHANCEMENTS_VIEW_LIGHT_SHAFTS), kCamera);
        check("with the pass off, the light-shafts view shows nothing, misses no entry, and says the pass did not run",
            report.view_shown() == View::off && report.missing.empty()
                && describe(report) == std::string(kAllSteps) + "; view light-shafts: light-shafts did not run");
        check("the light-shafts view wants the steps up to bloom", report.wanted.bits == (ENHANCEMENTS_STEP_GBUFFER | ENHANCEMENTS_STEP_COMBINE | ENHANCEMENTS_STEP_BLOOM));
    }
    {
        auto rows = walk;
        Settings settings = light_shafts_on(false);
        settings.view = View::light_shafts;
        const FrameReport report = run_frame(rows, settings, Inputs { true, SunCheck::unread });
        check("with the pass skipped, the light-shafts view says so too", report.view_shown() == View::off && describe(report)
            == std::string(kAllSteps) + "; skipped light-shafts: sun constants unread; view light-shafts: light-shafts did not run");
        FrameTracker tracker;
        fixture::replay(rows, tracker);
        FrameReport failed = FrameReport::start(settings);
        failed.note(plan(Step::bloom, tracker.map(), settings, kCamera));
        failed.note_failed(PassId::light_shafts);
        check("a light-shaft pass whose texture failed shows no view", !failed.note_view(Step::bloom, tracker.map()) && failed.view_shown() == View::off);
    }
    {
        auto rows = walk;
        fixture::find(rows, 1118).draw.inputs.resize(1);
        const FrameReport report = run_frame(rows, view_only(ENHANCEMENTS_VIEW_BLOOM_FINAL), kCamera);
        check("without a final bloom at the tonemap draw, the view says it is missing", report.view_shown() == View::off
            && report.missing.bits == ENHANCEMENTS_ENTRY_BLOOM_FINAL && describe(report) == std::string(kAllSteps) + "; view bloom-final: missing bloom-final");
    }
    {
        auto rows = walk;
        fixture::drop(rows, 884, 886);  // the quarter shadow's bind and draw
        const FrameReport report = run_frame(rows, view_only(ENHANCEMENTS_VIEW_QUARTER_SHADOW), kCamera);
        check("a view of a missing entry shows nothing and says what is missing", report.view_shown() == View::off
            && report.missing.bits == ENHANCEMENTS_ENTRY_QUARTER_SHADOW
            && describe(report) == "1152x720 found gbuffer shadow-mask ambient-pair combine bloom tonemap forward gbuffer-done tonemap-read; view quarter-shadow: missing quarter-shadow");
    }
    {
        auto rows = walk;
        fixture::drop(rows, 1100, 1200);  // everything from the tonemap on
        const FrameReport report = run_frame(rows, view_only(ENHANCEMENTS_VIEW_HDR_SCENE), kCamera);
        check("a view whose step never came names the step", report.view_shown() == View::off && report.missing.empty()
            && describe(report) == "1152x720 found gbuffer quarter-shadow shadow-mask ambient-pair combine bloom forward gbuffer-done; view hdr-scene: no tonemap");
    }
    {
        const auto glyphs = [](const Label &label) { return std::vector<int>(label.glyphs.begin(), label.glyphs.begin() + label.length); };
        check("the shadow-mask label spells SHADOW-MASK in font glyphs",
            glyphs(view_label(View::shadow_mask, true)) == std::vector<int> { 19, 8, 1, 4, 15, 23, 37, 13, 1, 19, 11 });
        check("a missing view's label adds a space and MISSING",
            glyphs(view_label(View::hdr_scene, false)) == std::vector<int> { 8, 4, 18, 37, 19, 3, 5, 14, 5, 0, 13, 9, 19, 19, 9, 14, 7 });
        check("digits, either case, a colon, a comma, and an unknown character as '?'", glyphs(encode_label("Ab09:,_")) == std::vector<int> { 1, 2, 27, 36, 39, 40, 38 });
        check("a view with nothing to show and a reason names it after a colon, cut at 32 glyphs",
            glyphs(view_label(View::trace, false, "scene stale")) == std::vector<int> { 20, 18, 1, 3, 5, 39, 0, 19, 3, 5, 14, 5, 0, 19, 20, 1, 12, 5 }
            && view_label(View::trace, false, std::string(40, 'x')).length == 32 && glyphs(view_label(View::trace, true, "scene stale")).size() == 5);
        check("a label stops at 32 glyphs", encode_label(std::string(40, 'z')).length == 32);
    }
    {
        FrameReport report = FrameReport::start(parse_settings(EnhancementsSettings { sizeof(EnhancementsSettings), 0, 0, 1, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {} }));
        check("disabled: describe says off", describe(report) == "off");
        report.error = ENHANCEMENTS_ERROR_SHADER;
        check("disabled by an error: describe says which", describe(report) == "off; error shader");
        report.compare_saved = 1;
        report.compare_count = 3;
        check("a comparison capture's progress comes before the error", describe(report) == "off; comparing 1/3; error shader");
        report.scene_saved = 2;
        report.scene_count = 7;
        check("a scene capture's progress follows the comparison's", describe(report) == "off; comparing 1/3; scene 2/7; error shader");
        report.scene_count = 0;
        report.scene_stopped = "the camera moved at +Y: it was 0.250 m from the first saved face's";
        check("a stopped scene capture says why",
            describe(report) == "off; comparing 1/3; scene stopped: the camera moved at +Y: it was 0.250 m from the first saved face's; error shader");
        report.frame_saved = "frame-20261024-101500";
        check("a saved frame names its folder after the scene capture",
            describe(report) == "off; comparing 1/3; scene stopped: the camera moved at +Y: it was 0.250 m from the first saved face's; saved frame-20261024-101500; error shader");
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
        report.error = ENHANCEMENTS_ERROR_TEXTURE;
        check("a pass whose texture failed is skipped",
            describe(report) == std::string(kAllSteps) + "; skipped ambient: no texture; error texture");
    }
    {
        EnhancementsSettings raw = raw_settings();
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
        EnhancementsSettings raw = raw_settings();
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
        EnhancementsSettings raw = raw_settings();
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
        EnhancementsSettings raw = raw_settings();
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
        EnhancementsSettings raw = raw_settings();
        raw.tonemap = { 1, -1.5f, ENHANCEMENTS_CURVE_NEUTRAL, 0.5f, 1.2f, 1.1f };
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
        raw.tonemap = { 1, -20, ENHANCEMENTS_CURVE_AGX, -1, -1, 0.1f };
        const Settings low = parse_settings(raw);
        check("tonemap clamps: -20 EV to -10, bloom and saturation -1 to 0, contrast 0.1 to 0.5, and keeps AgX", low.tonemap.exposure_ev == -10.0f
            && low.tonemap.curve == Curve::agx && low.tonemap.bloom_strength == 0.0f && low.tonemap.saturation == 0.0f && low.tonemap.contrast == 0.5f);
        raw.tonemap = { 1, 20, ENHANCEMENTS_CURVE_GAME, 9, 5, 3 };
        const Settings high = parse_settings(raw);
        check("tonemap clamps: 20 EV to 10, bloom 9 to 4, saturation 5 to 2, contrast 3 to 2", high.tonemap.exposure_ev == 10.0f
            && high.tonemap.bloom_strength == 4.0f && high.tonemap.saturation == 2.0f && high.tonemap.contrast == 2.0f);
    }
    {
        EnhancementsSettings raw = raw_settings();
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
        EnhancementsSettings raw = raw_settings();
        raw.sun_shadows = { 1, 0.05f, 0.1f, 0.8f };
        raw.wetness = { 1, 0.75f, 0.5f, 0.6f };
        const Settings whole = parse_settings(raw);
        check("a whole struct reads wetness: on, wetness 0.75, darkening 0.5, puddles 0.6", whole.wetness.enabled && whole.wetness.wetness == 0.75f
            && whole.wetness.darkening == 0.5f && whole.wetness.puddles == 0.6f);
        raw.size = 116;  // a caller built before wetness existed
        const Settings before = parse_settings(raw);
        check("a 116-byte struct reads its sun-shadow block and keeps wetness off, following the weather, darkening 0.35, puddles 0.3",
            !before.wetness.enabled && before.wetness.wetness == 0.0f && before.wetness.darkening == 0.35f && before.wetness.puddles == 0.3f
                && before.sun_shadows.enabled && before.sun_shadows.light_size == 0.05f);
        raw.size = 131;
        check("a struct one byte short of the wetness block keeps its defaults", !parse_settings(raw).wetness.enabled
            && parse_settings(raw).wetness.darkening == 0.35f);
        raw.size = sizeof raw;
        raw.wetness = { 1, NAN, INFINITY, -INFINITY };
        const Settings bad = parse_settings(raw);
        check("wetness: NaN and infinities fall back to wetness 0, darkening 0.35, puddles 0.3", bad.wetness.wetness == 0.0f
            && bad.wetness.darkening == 0.35f && bad.wetness.puddles == 0.3f);
        raw.wetness = { 1, -1, -0.5f, -2 };
        const Settings low = parse_settings(raw);
        check("wetness clamps: -1, -0.5 and -2 to 0", low.wetness.wetness == 0.0f && low.wetness.darkening == 0.0f && low.wetness.puddles == 0.0f);
        raw.wetness = { 1, 2, 1.5f, 3 };
        const Settings high = parse_settings(raw);
        check("wetness clamps: 2, 1.5 and 3 to 1", high.wetness.wetness == 1.0f && high.wetness.darkening == 1.0f && high.wetness.puddles == 1.0f);
    }
    {
        EnhancementsSettings raw = raw_settings();
        raw.wetness = { 1, 0.75f, 0.5f, 0.6f };
        raw.light_shafts = { 1, 2, 0.02f, 25, 0.8f, 300, 0.25f, 12, 0.5f, 96 };
        const LightShaftSettings whole = parse_settings(raw).light_shafts;
        check("a whole struct reads light shafts: on, strength 2, density 0.02, height 25, anisotropy 0.8, distance 300, "
              "patchiness 0.25, patch size 12, ambient 0.5, 96 steps", whole.enabled && whole.strength == 2.0f && whole.density == 0.02f
            && whole.height == 25.0f && whole.anisotropy == 0.8f && whole.distance == 300.0f && whole.patchiness == 0.25f
            && whole.patch_size == 12.0f && whole.ambient == 0.5f && whole.steps == 96);
        raw.size = 132;  // a caller built before light shafts existed
        const Settings before = parse_settings(raw);
        const LightShaftSettings &b = before.light_shafts;
        check("a 132-byte struct reads its wetness block and keeps light shafts off at strength 3, density 0.006, height 5, "
              "anisotropy 0.6, distance 150, patchiness 1, patch size 6, ambient 0.3, 48 steps", !b.enabled && b.strength == 3.0f
            && b.density == 0.006f && b.height == 5.0f && b.anisotropy == 0.6f && b.distance == 150.0f && b.patchiness == 1.0f
            && b.patch_size == 6.0f && b.ambient == 0.3f && b.steps == 48 && before.wetness.enabled && before.wetness.puddles == 0.6f);
        raw.size = 171;
        check("a struct one byte short of the light-shaft block keeps its defaults", !parse_settings(raw).light_shafts.enabled
            && parse_settings(raw).light_shafts.steps == 48);
        raw.size = sizeof raw;
        raw.light_shafts = { 1, NAN, INFINITY, -INFINITY, NAN, NAN, INFINITY, -INFINITY, NAN, 48 };
        const LightShaftSettings bad = parse_settings(raw).light_shafts;
        check("light shafts: NaN and infinities fall back to strength 3, density 0.006, height 5, anisotropy 0.6, distance 150, "
              "patchiness 1, patch size 6, ambient 0.3", bad.strength == 3.0f && bad.density == 0.006f && bad.height == 5.0f
            && bad.anisotropy == 0.6f && bad.distance == 150.0f && bad.patchiness == 1.0f && bad.patch_size == 6.0f && bad.ambient == 0.3f);
        raw.light_shafts = { 1, -1, -1, 0, -1, 0, -1, 0, -1, 0 };
        const LightShaftSettings low = parse_settings(raw).light_shafts;
        check("light shafts clamp: strength, density, patchiness and ambient -1 to 0, height 0 to 0.5, anisotropy -1 to -0.95, "
              "distance 0 to 5, patch size 0 to 0.5, 0 steps to 8", low.strength == 0.0f && low.density == 0.0f && low.height == 0.5f
            && low.anisotropy == -0.95f && low.distance == 5.0f && low.patchiness == 0.0f && low.patch_size == 0.5f && low.ambient == 0.0f
            && low.steps == 8);
        raw.light_shafts = { 1, 20, 1, 1000, 1, 2000, 2, 500, 9, 1000 };
        const LightShaftSettings high = parse_settings(raw).light_shafts;
        check("light shafts clamp: strength 20 to 10, density 1 to 0.2, height 1000 to 500, anisotropy 1 to 0.95, distance 2000 to "
              "900, patchiness 2 to 1, patch size 500 to 100, ambient 9 to 4, 1000 steps to 128", high.strength == 10.0f
            && high.density == 0.2f && high.height == 500.0f && high.anisotropy == 0.95f && high.distance == 900.0f && high.patchiness == 1.0f
            && high.patch_size == 100.0f && high.ambient == 4.0f && high.steps == 128);
        check("the default settings keep light shafts off at 48 steps", !Settings {}.light_shafts.enabled && Settings {}.light_shafts.steps == 48);
    }
    {
        const std::string raster = "; ran sun-shadows@combine contact-shadows@combine ambient@combine";
        auto rows = walk;
        const FrameReport none = run_frame(rows, trace_on(0), Inputs { true, SunCheck::ok, TraceState::none, false });
        check("without a traced scene the trace skips and the raster passes run as set", none.skipped.bits == ENHANCEMENTS_PASS_TRACE
            && none.replaced.empty() && describe(none) == std::string(kAllSteps) + raster + "; skipped trace: scene none");
        rows = walk;
        check("a stale scene says so", describe(run_frame(rows, trace_on(0), Inputs { true, SunCheck::ok, TraceState::stale, false }))
            == std::string(kAllSteps) + raster + "; skipped trace: scene stale");
        rows = walk;
        FrameReport ready = run_frame(rows, trace_on(0), Inputs { true, SunCheck::ok, TraceState::ready, false });
        check("a ready scene before its first full pass: the trace runs first and so does every raster pass",
            ready.ran.bits == (ENHANCEMENTS_PASS_TRACE | ENHANCEMENTS_PASS_SUN_SHADOWS | ENHANCEMENTS_PASS_CONTACT_SHADOWS | ENHANCEMENTS_PASS_AMBIENT)
            && describe(ready) == std::string(kAllSteps) + "; ran trace@combine (0 ao, 0 sun) sun-shadows@combine contact-shadows@combine ambient@combine");
        rows = walk;
        FrameReport traced = run_frame(rows, trace_on(0), Inputs { true, SunCheck::unread, TraceState::ready, true });
        traced.trace_ao = 64;
        traced.trace_sun = 32;
        check("once it writes the frame the three passes are replaced, not skipped, whatever their sun constants",
            traced.ran.bits == ENHANCEMENTS_PASS_TRACE && traced.skipped.empty() && traced.missing.empty()
            && traced.replaced.bits == (ENHANCEMENTS_PASS_SUN_SHADOWS | ENHANCEMENTS_PASS_CONTACT_SHADOWS | ENHANCEMENTS_PASS_AMBIENT)
            && describe(traced) == std::string(kAllSteps) + "; ran trace@combine (64 ao, 32 sun); replaced sun-shadows contact-shadows ambient");
        rows = walk;
        FrameReport ao = run_frame(rows, trace_on(ENHANCEMENTS_TRACE_AO), Inputs { true, SunCheck::ok, TraceState::ready, true });
        check("traced occlusion alone replaces ambient alone", ao.replaced.bits == ENHANCEMENTS_PASS_AMBIENT
            && describe(ao) == std::string(kAllSteps) + "; ran trace@combine (0 ao, 0 sun) sun-shadows@combine contact-shadows@combine; replaced ambient");
        rows = walk;
        FrameReport sun = run_frame(rows, trace_on(ENHANCEMENTS_TRACE_SUN), Inputs { true, SunCheck::ok, TraceState::ready, true });
        check("traced sun alone replaces sun shadows and contact shadows", sun.replaced.bits == (ENHANCEMENTS_PASS_SUN_SHADOWS | ENHANCEMENTS_PASS_CONTACT_SHADOWS)
            && sun.ran.bits == (ENHANCEMENTS_PASS_TRACE | ENHANCEMENTS_PASS_AMBIENT));
        rows = walk;
        check("without a camera the trace skips like the others", describe(run_frame(rows, trace_on(0), Inputs { false, SunCheck::ok, TraceState::ready, false }))
            == std::string(kAllSteps) + "; skipped trace: no camera; skipped sun-shadows: no camera; skipped contact-shadows: no camera; skipped ambient: no camera");
        Settings alone = view_only(ENHANCEMENTS_VIEW_OFF);
        alone.trace.enabled = true;
        check("the trace wants the combine step and its prerequisites, and no sun constants", wanted_steps(alone).has(Step::combine) && !wants_sun(alone));
        rows = walk;
        std::vector<Step> snapshots;
        const FrameReport view = run_frame(rows, trace_on(0, ENHANCEMENTS_VIEW_TRACE), Inputs { true, SunCheck::ok, TraceState::ready, false }, &snapshots);
        check("the trace view is copied at the combine step from the pass's own result", snapshots == std::vector<Step> { Step::combine }
            && view.view_shown() == View::trace && view_label(View::trace, true).length == 5);
        rows = walk;
        const FrameReport building = run_frame(rows, trace_on(0, ENHANCEMENTS_VIEW_TRACE), Inputs { true, SunCheck::ok, TraceState::building, false });
        check("and without a scene the view says the trace did not run",
            describe(building) == std::string(kAllSteps) + raster + "; skipped trace: scene building; view trace: trace did not run");
        const auto text = [](const Label &label) {
            static const char kFont[] = " ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-?:,";
            std::string out;
            for (uint8_t i = 0; i < label.length; i++)
                out += kFont[label.glyphs[i]];
            return out;
        };
        check("its label says why, as the status line does: TRACE: SCENE BUILDING", view_missing_why(building) == "scene building"
            && text(view_label(building.view, building.view_found, view_missing_why(building))) == "TRACE: SCENE BUILDING");
        rows = walk;
        const FrameReport stale = run_frame(rows, trace_on(0, ENHANCEMENTS_VIEW_TRACE), Inputs { true, SunCheck::ok, TraceState::stale, false });
        rows = walk;
        const FrameReport cameraless = run_frame(rows, trace_on(0, ENHANCEMENTS_VIEW_TRACE), Inputs { false, SunCheck::ok, TraceState::none, false });
        rows = walk;
        const FrameReport off = run_frame(rows, view_only(ENHANCEMENTS_VIEW_TRACE), Inputs { true, SunCheck::ok, TraceState::ready, false });
        std::printf("INFO labels: %s | %s | %s\n", text(view_label(stale.view, stale.view_found, view_missing_why(stale))).c_str(),
            text(view_label(cameraless.view, cameraless.view_found, view_missing_why(cameraless))).c_str(), text(view_label(off.view, off.view_found, view_missing_why(off))).c_str());
        check("a stale scene, no camera and no scene, and the trace off, each in the label",
            text(view_label(stale.view, stale.view_found, view_missing_why(stale))) == "TRACE: SCENE STALE"
                && text(view_label(cameraless.view, cameraless.view_found, view_missing_why(cameraless))) == "TRACE: NO CAMERA, SCENE NONE"
                && text(view_label(off.view, off.view_found, view_missing_why(off))) == "TRACE: OFF");
        check("a found view, and a game entry's view, have no reason", view_missing_why(view).empty()
            && view_missing_why(run_frame(rows = walk, view_only(ENHANCEMENTS_VIEW_SHADOW_MASK), kCamera)).empty());
        rows = walk;
        FrameReport gathered = run_frame(rows, trace_on(0), Inputs { true, SunCheck::ok, TraceState::ready, true });
        gathered.trace_gather = 128;
        gathered.trace_sun = 32;
        check("while it gathers, the trace names its gather samples in place of its occlusion samples",
            describe(gathered) == std::string(kAllSteps) + "; ran trace@combine (128 gather, 32 sun); replaced sun-shadows contact-shadows ambient");
        rows = walk;
        FrameReport unlit = run_frame(rows, trace_on(0), Inputs { true, SunCheck::ok, TraceState::ready, true });
        unlit.trace_ao = 64;
        unlit.trace_sun = 32;
        unlit.gather_without_light = true;
        check("the gather over a scene captured without light says so after the trace's outputs, which run as before",
            describe(unlit) == std::string(kAllSteps) + "; ran trace@combine (64 ao, 32 sun); replaced sun-shadows contact-shadows ambient; skipped gather: scene without light");
        for (const auto &[view, name] : { std::pair { ENHANCEMENTS_VIEW_BOUNCE, "bounce" }, std::pair { ENHANCEMENTS_VIEW_SKY_VISIBILITY, "sky-visibility" },
                 std::pair { ENHANCEMENTS_VIEW_SKY_MAP, "sky-map" } })
        {
            rows = walk;
            std::vector<Step> copied;
            const FrameReport shown = run_frame(rows, trace_on(0, view), Inputs { true, SunCheck::ok, TraceState::ready, false }, &copied);
            check((std::string("the ") + name + " view is the trace pass's own, copied at the combine step").c_str(), copied == std::vector<Step> { Step::combine }
                && shown.view_shown() == static_cast<View>(view) && kViews[view].name == std::string(name) && kViews[view].from == PassId::trace);
        }
        check("the bounce and sky-map views are tone mapped, the sky visibility shown as it is", kViews[ENHANCEMENTS_VIEW_BOUNCE].decode == Decode::hdr
            && kViews[ENHANCEMENTS_VIEW_SKY_MAP].decode == Decode::hdr && kViews[ENHANCEMENTS_VIEW_SKY_VISIBILITY].decode == Decode::rgb);
        check("only sun shadows, contact shadows and ambient are replaced, each by its output", replaced_by_trace(PassId::sun_shadows, alone.trace)
            && replaced_by_trace(PassId::ambient, alone.trace) && !replaced_by_trace(PassId::atmosphere, alone.trace)
            && !replaced_by_trace(PassId::ambient, trace_on(ENHANCEMENTS_TRACE_SUN).trace));
    }
    {
        EnhancementsSettings raw = raw_settings();
        raw.trace = { 1, ENHANCEMENTS_TRACE_SUN, ENHANCEMENTS_TRACE_CAST_ALPHA_CARDS, 5, 0.5f, 0.03f, 0.75f, 16, 8, 30 };
        const TraceSettings whole = parse_settings(raw).trace;
        check("a whole struct reads the trace: on, sun only, alpha cards but no characters, 5 m, strength 0.5, sun 0.03 at 0.75, "
              "16 and 8 samples, 30 ms", whole.enabled && !whole.ao && whole.sun && !whole.characters && whole.alpha_cards
            && whole.ao_distance == 5.0f && whole.ao_strength == 0.5f && whole.sun_size == 0.03f && whole.sun_strength == 0.75f
            && whole.ao_samples == 16 && whole.sun_samples == 8 && whole.budget_ms == 30.0f);
        raw.trace.outputs = 0;
        raw.trace.casters = 0xFFFFFFFFu;
        const TraceSettings all = parse_settings(raw).trace;
        check("outputs 0 reads as both, and casters keep their known bits", all.ao && all.sun && all.characters && all.alpha_cards && all.whole_cards);
        raw.size = 172;     // a caller of version 7
        const TraceSettings before = parse_settings(raw).trace;
        check("a 172-byte struct keeps the trace off at 2 m, 0.0093, 64 and 32 samples and 100 ms, with characters and cards casting",
            !before.enabled && before.ao && before.sun && before.characters && before.alpha_cards && !before.whole_cards && before.ao_distance == 2.0f
            && before.ao_strength == 1.0f && before.sun_size == 0.0093f && before.sun_strength == 1.0f && before.ao_samples == 64
            && before.sun_samples == 32 && before.budget_ms == 100.0f);
        raw.size = 211;
        check("a struct one byte short of the trace block keeps its defaults", !parse_settings(raw).trace.enabled);
        raw.size = sizeof raw;
        raw.trace = { 1, 0, 0, NAN, INFINITY, -INFINITY, NAN, 64, 32, NAN };
        const TraceSettings bad = parse_settings(raw).trace;
        check("the trace: NaN and infinities fall back to 2 m, strength 1, 0.0093, 1 and 100 ms", bad.ao_distance == 2.0f && bad.ao_strength == 1.0f
            && bad.sun_size == 0.0093f && bad.sun_strength == 1.0f && bad.budget_ms == 100.0f);
        raw.trace = { 1, 0, 0, 0, -1, -1, -1, 0, 0, 0 };
        const TraceSettings low = parse_settings(raw).trace;
        check("the trace clamps: distance 0 to 0.1, strengths and sun size -1 to 0, 0 samples to 1, 0 ms to 2", low.ao_distance == 0.1f
            && low.ao_strength == 0.0f && low.sun_size == 0.0f && low.sun_strength == 0.0f && low.ao_samples == 1 && low.sun_samples == 1
            && low.budget_ms == 2.0f);
        raw.trace = { 1, 0, 0, 100, 2, 1, 2, 10000, 10000, 1000 };
        const TraceSettings high = parse_settings(raw).trace;
        check("the trace clamps: distance 100 to 20, strengths 2 to 1, sun size 1 to 0.2, 10000 samples to 4096, 1000 ms to 200",
            high.ao_distance == 20.0f && high.ao_strength == 1.0f && high.sun_size == 0.2f && high.sun_strength == 1.0f && high.ao_samples == 4096
            && high.sun_samples == 4096 && high.budget_ms == 200.0f);
    }
    {
        EnhancementsSettings raw = raw_settings();
        raw.gather = { 1, 256, ENHANCEMENTS_GATHER_SKY_MAP, 1, 2, 0.25f };
        const GatherSettings whole = parse_settings(raw).gather;
        check("a whole struct reads the gather: on, 256 samples, the sky map, one filter pass, strength 2, a quarter of the ambient light",
            whole.enabled && whole.samples == 256 && whole.sky == GatherSky::map && whole.filter == 1 && whole.strength == 2.0f
                && whole.hit_ambient == 0.25f);
        raw.size = 212;     // a caller of version 8
        const GatherSettings before = parse_settings(raw).gather;
        check("a 212-byte struct keeps the gather off at 64 samples, visibility, two filter passes, strength 1 and half the ambient light",
            !before.enabled && before.samples == 64 && before.sky == GatherSky::visibility && before.filter == 2 && before.strength == 1.0f
                && before.hit_ambient == 0.5f && parse_settings(raw).trace.enabled == parse_settings(raw_settings()).trace.enabled);
        raw.size = sizeof raw;
        raw.gather = { 1, 0, 9, 7, NAN, -1 };
        const GatherSettings odd = parse_settings(raw).gather;
        check("the gather clamps: 0 samples to 1, an unknown sky to visibility, filter 7 to 2, a NaN strength to 1, hit_ambient -1 to 0",
            odd.samples == 1 && odd.sky == GatherSky::visibility && odd.filter == 2 && odd.strength == 1.0f && odd.hit_ambient == 0.0f);
        raw.gather = { 1, 100000, 0, 0, 100, 5 };
        const GatherSettings high = parse_settings(raw).gather;
        check("and 100000 samples to 4096, strength 100 to 4, hit_ambient 5 to 1, the occlusion sky and no filter kept", high.samples == 4096
            && high.strength == 4.0f && high.hit_ambient == 1.0f && high.sky == GatherSky::occlusion && high.filter == 0);
    }
    {
        EnhancementsSettings raw = raw_settings();
        raw.depth_of_field = { 1, 1.4f, 36, 3, { 0.25f, 0.75f }, 0.5f, 0.05f, 7, 0.2f, 15, 1, 0.6f, 0.3f, 1.33f, 1, 256, 3, 1.5f, 1, 0.75f, 1, 0.42f, 0.02f };
        const DepthOfFieldSettings whole = parse_settings(raw).depth_of_field;
        check("a whole 396-byte struct reads the depth of field: on, f/1.4 on a 36 mm sensor focused at 3 m, its point, speed, blur, 7 blades, "
              "highlights from 3 times the mean at gain 1.5, scatter, bokeh contrast 0.75, on the display, an eye 0.42 m away in a 0.02 m window, and the rest",
            sizeof raw == 396 && whole.eye_distance == 0.42f && whole.eye_size == 0.02f && whole.place == DofPlace::display && whole.scatter && whole.bokeh_contrast == 0.75f && whole.enabled && whole.f_number == 1.4f && whole.sensor_height_mm == 36 && whole.focus_distance == 3
                && whole.focus_point[0] == 0.25f && whole.focus_point[1] == 0.75f && whole.focus_seconds == 0.5f && whole.max_blur == 0.05f
                && whole.blades == 7 && whole.blade_roundness == 0.2f && whole.blade_rotation == 15 && whole.spherical_aberration == 1
                && whole.cat_eye == 0.6f && whole.swirl == 0.3f && whole.anamorphic == 1.33f && whole.chromatic == 1 && whole.samples == 256
                && whole.highlight_threshold == 3 && whole.highlight_gain == 1.5f);
        raw.size = 324;     // a caller of version 15
        const DepthOfFieldSettings fifteen = parse_settings(raw).depth_of_field;
        check("a 324-byte struct reads the place but has no eye, and a window of 0.015 m", fifteen.place == DofPlace::display && fifteen.eye_distance == 0
            && fifteen.eye_size == 0.015f);
        raw.size = 320;     // a caller of version 14
        const DepthOfFieldSettings fourteen = parse_settings(raw).depth_of_field;
        check("a 320-byte struct reads the bokeh contrast but keeps the place in the scene", fourteen.bokeh_contrast == 0.75f && fourteen.place == DofPlace::scene);
        raw.size = sizeof raw;
        raw.depth_of_field.place = 7;
        check("an unknown place reads as the scene", parse_settings(raw).depth_of_field.place == DofPlace::scene);
        raw.size = 316;     // a caller of version 13
        const DepthOfFieldSettings thirteen = parse_settings(raw).depth_of_field;
        check("a 316-byte struct reads the scatter but keeps the bokeh contrast off", thirteen.scatter && thirteen.bokeh_contrast == 0);
        raw.size = 312;     // a caller of version 12
        const DepthOfFieldSettings twelve = parse_settings(raw).depth_of_field;
        check("a 312-byte struct reads the highlights but keeps the scatter off", twelve.highlight_threshold == 3 && twelve.highlight_gain == 1.5f
            && !twelve.scatter);
        raw.size = 304;     // a caller of version 11
        const DepthOfFieldSettings eleven = parse_settings(raw).depth_of_field;
        check("a 304-byte struct reads the depth of field but keeps the highlights at 2 times the mean and gain 2",
            eleven.enabled && eleven.f_number == 1.4f && eleven.samples == 256 && eleven.highlight_threshold == 2 && eleven.highlight_gain == 2);
        raw.size = 236;     // a caller of version 10
        const DepthOfFieldSettings before = parse_settings(raw).depth_of_field;
        check("a 236-byte struct keeps the depth of field off at f/2.8, 24 mm, autofocus at the center, 2 seconds of focus, blur 0.03, round, "
              "spherical aberration 0.6 and 49 samples",
            !before.enabled && before.f_number == 2.8f && before.sensor_height_mm == 24 && before.focus_distance == 0 && before.focus_point[0] == 0.5f
                && before.focus_point[1] == 0.5f && before.focus_seconds == 2 && before.max_blur == 0.03f && before.blades == 0
                && before.spherical_aberration == 0.6f && before.anamorphic == 1 && before.chromatic == 0 && before.samples == 49);
        raw.size = sizeof raw;
        raw.depth_of_field = { 1, 0, 1, -5, { -1, 2 }, 50, 1, 2, -1, 1000, -3, 7, -1, 0.1f, 9, 0, 0.5f, -1, 7, 3, 0, -2, 1 };
        const DepthOfFieldSettings low = parse_settings(raw).depth_of_field;
        check("the depth of field clamps: f/0 to f/1, a 1 mm sensor to 4, focus -5 to autofocus, the point into the picture, 50 seconds of focus to 10, "
              "blur 1 to 0.1, 2 blades to 5, rotation 1000 to 360, aberration -3 to -1, anamorphic 0.1 to 0.5, chromatic 9 to 5, 0 samples to 9, threshold 0.5 to 1, gain -1 to 0, bokeh contrast 3 to 2, an eye -2 m away to none and a 1 m window to 0.1",
            low.f_number == 1 && low.sensor_height_mm == 4 && low.focus_distance == 0 && low.focus_point[0] == 0 && low.focus_point[1] == 1
                && low.focus_seconds == 10 && low.max_blur == 0.1f && low.blades == 5 && low.blade_roundness == 0 && low.blade_rotation == 360
                && low.spherical_aberration == -1 && low.cat_eye == 1 && low.swirl == 0 && low.anamorphic == 0.5f && low.chromatic == 5 && low.samples == 9
                && low.highlight_threshold == 1 && low.highlight_gain == 0 && low.scatter && low.bokeh_contrast == 2 && low.eye_distance == 0 && low.eye_size == 0.1f);
        raw.depth_of_field = { 1, NAN, INFINITY, NAN, { NAN, NAN }, NAN, NAN, 99, NAN, NAN, NAN, NAN, NAN, NAN, NAN, 5000, NAN, INFINITY, 1, NAN, 0, NAN, NAN };
        const DepthOfFieldSettings bad = parse_settings(raw).depth_of_field;
        check("NaN and infinities fall back to the defaults, 99 blades to 16 and 5000 samples to 1089", bad.f_number == 2.8f && bad.sensor_height_mm == 24
            && bad.focus_point[0] == 0.5f && bad.focus_seconds == 2 && bad.max_blur == 0.03f && bad.blades == 16 && bad.spherical_aberration == 0.6f
            && bad.anamorphic == 1 && bad.samples == 1089 && bad.highlight_threshold == 2 && bad.highlight_gain == 2 && bad.bokeh_contrast == 0 && bad.eye_distance == 0 && bad.eye_size == 0.015f);
        raw.depth_of_field.sensor_height_mm = 12000;
        const float miniature = parse_settings(raw).depth_of_field.sensor_height_mm;
        raw.depth_of_field.sensor_height_mm = 50000;
        check("a 12000 mm sensor, a 1:500 miniature's, stays and a 50000 mm one clamps to 20000",
            miniature == 12000 && parse_settings(raw).depth_of_field.sensor_height_mm == 20000);
        raw.depth_of_field.focus_seconds = 0.1f;
        const float fast = parse_settings(raw).depth_of_field.focus_seconds;
        raw.depth_of_field.focus_seconds = -1;
        check("0.1 seconds of focus stays and -1 clamps to 0, which jumps", fast == 0.1f && parse_settings(raw).depth_of_field.focus_seconds == 0);
        check("the depth of field runs last at the bloom step and wants it", wanted_steps(parse_settings(raw)).has(Step::bloom)
            && kPasses[7].id == PassId::depth_of_field && std::string(kPasses[7].name) == "depth-of-field");
    }
    {
        EnhancementsCamera raw {};
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
        check("a camera with nothing in sky_color[3] brings a dry weather", bad.wetness == 0.0f);
        raw.sky_color[3] = 0.25f;
        check("sky_color[3] 0.25 is the weather's wetness", parse_camera(raw).wetness == 0.25f);
        raw.sky_color[3] = 3;
        check("a weather wetness of 3 clamps to 1", parse_camera(raw).wetness == 1.0f);
        raw.sky_color[3] = -1;
        check("a weather wetness of -1 clamps to 0", parse_camera(raw).wetness == 0.0f);
        raw.sky_color[3] = NAN;
        check("a NaN weather wetness reads as dry", parse_camera(raw).wetness == 0.0f);
    }
    {
        check("disabled settings want no step", wanted_steps(view_only(ENHANCEMENTS_VIEW_HDR_SCENE)).bits != 0
            && wanted_steps(parse_settings(EnhancementsSettings { sizeof(EnhancementsSettings), 0, ENHANCEMENTS_VIEW_HDR_SCENE, 1, { 1, 1, 1, 1, 1 }, { 1, 1, 1, 1, 1 }, { 1, 1, 1, 1, 1 }, { 1, 1, 0, 1, 1, 1 }, { 1, 1, 1, 1 }, { 1, 1, 1, 1 }, { 1, 1, 1, 1, 1, 1, 1, 1, 1, 8 }, { 1, 1, 1, 1, 1, 1, 1, 1, 1, 2 }, { 1, 1, 1, 1, 1, 1 }, { 1, 2, 24, 0, { 0.5f, 0.5f }, 0, 0.03f, 0, 0, 0, 0, 0, 0, 1, 0, 49, 2, 2, 1, 1, 0, 0, 0.015f }, {} })).empty());
    }
    {
        // A traced shot over the applied settings.
        EnhancementsSettings raw = raw_settings();
        raw.view = ENHANCEMENTS_VIEW_NORMALS;
        raw.tonemap.enabled = 1;
        raw.gather.samples = 32;
        raw.trace.budget_ms = 100;
        const Settings on = parse_settings(raw);
        raw.enabled = 0;
        const Settings off = parse_settings(raw);
        EnhancementsShot shot = { sizeof shot, 1, 0, 0 };
        const Settings same = with_shot(on, parse_shot(nullptr)), plain = with_shot(on, parse_shot(&shot));
        check("no shot leaves the applied settings as they are", !same.trace.enabled && !same.gather.enabled && same.view == View::normals && same.ambient.enabled);
        check("a shot switches the trace and the gather on over applied settings that are on, and keeps their passes, view, samples and budget",
            plain.enabled && plain.trace.enabled && plain.gather.enabled && plain.ambient.enabled && plain.tonemap.enabled && plain.view == View::normals
            && plain.gather.samples == 32 && plain.trace.budget_ms == 100.0f && plain.ambient.level == 0.4f);
        const Settings alone = with_shot(off, parse_shot(&shot));
        check("under a master switch that is off the shot runs alone: the trace and the gather, no other pass, no view, the applied ambient level",
            alone.enabled && alone.trace.enabled && alone.gather.enabled && !alone.ambient.enabled && !alone.tonemap.enabled && alone.view == View::off
            && alone.ambient.level == 0.4f && wanted_steps(alone).has(Step::combine));
        shot.samples = 128;
        shot.budget_ms = 150;
        const Settings given = with_shot(on, parse_shot(&shot));
        check("a shot's samples and budget replace the applied ones", given.gather.samples == 128 && given.trace.budget_ms == 150.0f);
        shot.samples = 100000;
        shot.budget_ms = 1;
        check("a shot's samples clamp to 4096 and its budget to 2 ms", parse_shot(&shot).samples == 4096 && parse_shot(&shot).budget_ms == 2.0f);
        shot.budget_ms = NAN;
        check("a NaN budget keeps the applied one", parse_shot(&shot).budget_ms == 0.0f && with_shot(on, parse_shot(&shot)).trace.budget_ms == 100.0f);
        shot.size = 8;
        check("an 8-byte shot is active with the applied samples and budget", parse_shot(&shot).active && parse_shot(&shot).samples == 0);
        shot.size = 4;
        check("a shot too short for active, an inactive one and a null one are no shot", !parse_shot(&shot).active && !parse_shot(nullptr).active
            && (shot = { sizeof shot, 0, 64, 100 }, !with_shot(on, parse_shot(&shot)).trace.enabled));
        // The status as a caller of each version reads it: it gets the bytes of its own size.
        check("the status is 160 bytes: the restarts at 132, in a version 9 caller's padding, their reason at 136, the shot at 140, the saved frames "
              "at 144, the focus at 148 and the flare's visibility at 152, and the shot is 16, in version 17",
            sizeof(EnhancementsStatus) == 160 && offsetof(EnhancementsStatus, flare_visibility) == 152 && offsetof(EnhancementsStatus, trace_light) == 128 && offsetof(EnhancementsStatus, trace_restarts) == 132
            && offsetof(EnhancementsStatus, trace_restart_reason) == 136 && offsetof(EnhancementsStatus, shot_active) == 140
            && offsetof(EnhancementsStatus, frames_saved) == 144 && offsetof(EnhancementsStatus, dof_focus_meters) == 148 && sizeof(EnhancementsShot) == 16 && ENHANCEMENTS_VERSION == 17);
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
