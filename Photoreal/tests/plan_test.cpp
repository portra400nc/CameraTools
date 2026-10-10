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
        raw.ambient = { 1, 0.4f, 0.8f, 1.5f };
        return raw;
    }

    Settings ambient_on() { return parse_settings(raw_settings()); }

    Settings view_only(uint32_t view)
    {
        PhotorealSettings raw = raw_settings();
        raw.view = view;
        raw.ambient.enabled = 0;
        return parse_settings(raw);
    }

    // Replays a fixture as the shell does: at every moment, plan the passes, note the plan and the view; finish at the end.
    FrameReport run_frame(std::vector<fixture::Row> &rows, const Settings &settings, bool camera_known, std::vector<Step> *snapshots = nullptr)
    {
        FrameTracker tracker;
        FrameReport report = FrameReport::start(settings);
        tracker.begin_frame();
        for (fixture::Row &row : rows)
        {
            const std::optional<Step> step = tracker.feed(fixture::event(row));
            if (!step)
                continue;
            report.note(plan(*step, tracker.map(), settings, camera_known));
            if (report.note_view(*step, tracker.map()) && snapshots)
                snapshots->push_back(*step);
        }
        report.finish(tracker, settings, camera_known);
        return report;
    }

    const char *kAllSteps = "1152x720 found gbuffer quarter-shadow shadow-mask ambient-pair combine bloom tonemap";
}

int main()
{
    const std::vector<fixture::Row> walk = fixture::load("fixtures/capture-20261009-214859.tsv");
    {
        auto rows = walk;
        const FrameReport report = run_frame(rows, ambient_on(), true);
        check("ambient runs at combine", report.ran.bits == PHOTOREAL_PASS_AMBIENT && report.skipped.empty());
        check("describe names every step and the pass", describe(report) == std::string(kAllSteps) + "; ran ambient@combine");
        check("steps wanted by ambient: gbuffer and combine", report.wanted.bits == (PHOTOREAL_STEP_GBUFFER | PHOTOREAL_STEP_COMBINE));
    }
    {
        auto rows = walk;
        fixture::drop(rows, 910, 913);
        const FrameReport report = run_frame(rows, ambient_on(), true);
        check("without the ambient pair, ambient skips", report.skipped.bits == PHOTOREAL_PASS_AMBIENT && report.ran.empty());
        check("and reports the missing entry", report.missing.bits == PHOTOREAL_ENTRY_AMBIENT_DIFFUSE);
        check("describe says why",
            describe(report) == "1152x720 found gbuffer quarter-shadow shadow-mask combine bloom tonemap; skipped ambient: missing ambient-diffuse");
    }
    {
        auto rows = walk;
        const FrameReport report = run_frame(rows, ambient_on(), false);
        check("without a camera, ambient skips and says so", report.skipped.bits == PHOTOREAL_PASS_AMBIENT
            && describe(report) == std::string(kAllSteps) + "; skipped ambient: no camera");
    }
    {
        std::vector<fixture::Row> menu;  // a frame with no G-buffer: a loading screen or a menu
        const FrameReport report = run_frame(menu, ambient_on(), true);
        check("no G-buffer: nothing runs", report.ran.empty() && report.skipped.bits == PHOTOREAL_PASS_AMBIENT);
        check("no G-buffer: describe says so", describe(report) == "found nothing; skipped ambient: missing normals depth ambient-diffuse");
        check("no G-buffer: status bits name the missing entries",
            report.missing.bits == (PHOTOREAL_ENTRY_NORMALS | PHOTOREAL_ENTRY_DEPTH | PHOTOREAL_ENTRY_AMBIENT_DIFFUSE));
    }
    {
        auto rows = walk;
        fixture::drop(rows, 924, 926);  // the HDR scene's bind and clear: every entry ambient needs, but no combine moment
        const FrameReport report = run_frame(rows, ambient_on(), true);
        check("no combine moment: ambient skips and names the step",
            describe(report) == "1152x720 found gbuffer quarter-shadow shadow-mask ambient-pair; skipped ambient: no combine");
    }
    {
        auto rows = walk;
        std::vector<Step> snapshots;
        const FrameReport report = run_frame(rows, view_only(PHOTOREAL_VIEW_NORMALS), true, &snapshots);
        check("the normals view snapshots once, at combine", snapshots == std::vector<Step> { Step::combine });
        check("the normals view is shown", report.view_shown() == View::normals);
        check("describe names the view", describe(report) == std::string(kAllSteps) + "; view normals");
    }
    {
        auto rows = walk;
        std::vector<Step> snapshots;
        const FrameReport report = run_frame(rows, view_only(PHOTOREAL_VIEW_HDR_SCENE), true, &snapshots);
        check("the HDR view snapshots at tonemap", snapshots == std::vector<Step> { Step::tonemap } && report.view_shown() == View::hdr_scene);
        check("the HDR view wants the steps up to tonemap",
            report.wanted.bits == (PHOTOREAL_STEP_GBUFFER | PHOTOREAL_STEP_COMBINE | PHOTOREAL_STEP_TONEMAP));
    }
    {
        auto rows = walk;
        fixture::drop(rows, 884, 886);  // the quarter shadow's bind and draw
        const FrameReport report = run_frame(rows, view_only(PHOTOREAL_VIEW_QUARTER_SHADOW), true);
        check("a view of a missing entry shows nothing and says what is missing", report.view_shown() == View::off
            && report.missing.bits == PHOTOREAL_ENTRY_QUARTER_SHADOW
            && describe(report) == "1152x720 found gbuffer shadow-mask ambient-pair combine bloom tonemap; view quarter-shadow: missing quarter-shadow");
    }
    {
        auto rows = walk;
        fixture::drop(rows, 1100, 1200);  // everything from the tonemap on
        const FrameReport report = run_frame(rows, view_only(PHOTOREAL_VIEW_HDR_SCENE), true);
        check("a view whose step never came names the step", report.view_shown() == View::off && report.missing.empty()
            && describe(report) == "1152x720 found gbuffer quarter-shadow shadow-mask ambient-pair combine bloom; view hdr-scene: no tonemap");
    }
    {
        FrameReport report = FrameReport::start(parse_settings(PhotorealSettings { sizeof(PhotorealSettings), 0, 0, 1, {} }));
        check("disabled: describe says off", describe(report) == "off");
        report.error = PHOTOREAL_ERROR_SHADER;
        check("disabled by an error: describe says which", describe(report) == "off; error shader");
    }
    {
        auto rows = walk;
        FrameReport report = run_frame(rows, ambient_on(), true);
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
        raw.ambient = { 1, 2, 0.5f, 2 };
        const Settings s = parse_settings(raw);
        check("a short struct reads what it holds", s.enabled);
        check("a short struct keeps later fields at their defaults", s.view == View::off && s.flip && !s.ambient.enabled
            && s.ambient.level == 1.0f && s.ambient.ao_strength == 1.0f && s.ambient.ao_radius == 1.0f);
        raw.size = 4;
        check("a struct too short for enabled reads as off", !parse_settings(raw).enabled);
    }
    {
        PhotorealSettings raw = raw_settings();
        raw.view = 99;
        raw.flip = 0;
        raw.ambient = { 1, NAN, -3, INFINITY };
        const Settings s = parse_settings(raw);
        check("an unknown view reads as off", s.view == View::off);
        check("flip 0 reads as upright", !s.flip);
        check("NaN and infinity fall back to the defaults, and -3 clamps to 0",
            s.ambient.level == 1.0f && s.ambient.ao_strength == 0.0f && s.ambient.ao_radius == 1.0f);
        raw.ambient = { 1, 9, 2, 0.001f };
        const Settings c = parse_settings(raw);
        check("out-of-range floats clamp: level 4, strength 1, radius 0.05",
            c.ambient.level == 4.0f && c.ambient.ao_strength == 1.0f && c.ambient.ao_radius == 0.05f);
    }
    {
        check("disabled settings want no step", wanted_steps(view_only(PHOTOREAL_VIEW_HDR_SCENE)).bits != 0
            && wanted_steps(parse_settings(PhotorealSettings { sizeof(PhotorealSettings), 0, PHOTOREAL_VIEW_HDR_SCENE, 1, { 1, 1, 1, 1 } })).empty());
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
