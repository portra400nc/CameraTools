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
        const FrameReport report = run_frame(rows, contact_shadows_on(true), true);
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
        const Plan p = plan(Step::combine, tracker.map(), contact_shadows_on(true), true);
        check("the combine plan runs contact shadows first, then ambient", p.count == 2
            && p.items[0].pass == PassId::contact_shadows && p.items[0].kind == Decision::Kind::run
            && p.items[1].pass == PassId::ambient && p.items[1].kind == Decision::Kind::run);
        check("no other step has a pass", plan(Step::shadow_mask, tracker.map(), contact_shadows_on(true), true).count == 0
            && plan(Step::tonemap, tracker.map(), contact_shadows_on(true), true).count == 0);
    }
    {
        auto rows = fixture::load("fixtures/capture-20261010-111824.tsv");
        const FrameReport report = run_frame(rows, contact_shadows_on(true), true);
        check("111824, the user's normal settings: contact shadows and ambient both run", describe(report)
            == "1920x1200 found gbuffer quarter-shadow shadow-mask ambient-pair combine bloom tonemap; ran contact-shadows@combine ambient@combine");
    }
    {
        auto rows = walk;
        fixture::drop(rows, 892, 892);  // the shadow mask's clear, so no shadow mask is found
        const FrameReport report = run_frame(rows, contact_shadows_on(true), true);
        check("without the shadow mask, contact shadows skip and ambient runs",
            report.skipped.bits == PHOTOREAL_PASS_CONTACT_SHADOWS && report.ran.bits == PHOTOREAL_PASS_AMBIENT);
        check("and the shadow mask is the missing entry", report.missing.bits == PHOTOREAL_ENTRY_SHADOW_MASK);
        check("describe says why", describe(report)
            == "1152x720 found gbuffer quarter-shadow ambient-pair combine bloom tonemap; ran ambient@combine; skipped contact-shadows: missing shadow-mask");
    }
    {
        auto rows = walk;
        const FrameReport report = run_frame(rows, contact_shadows_on(true), false);
        check("without a camera, both passes skip and say so", report.skipped.bits == (PHOTOREAL_PASS_CONTACT_SHADOWS | PHOTOREAL_PASS_AMBIENT)
            && describe(report) == std::string(kAllSteps) + "; skipped contact-shadows: no camera; skipped ambient: no camera");
    }
    {
        std::vector<fixture::Row> menu;
        const FrameReport report = run_frame(menu, contact_shadows_on(false), true);
        check("no G-buffer: contact shadows name the entries they need",
            describe(report) == "found nothing; skipped contact-shadows: missing normals depth shadow-mask");
    }
    {
        auto rows = walk;
        std::vector<Step> snapshots;
        const FrameReport report = run_frame(rows, contact_shadows_on(false, PHOTOREAL_VIEW_SHADOW_MASK), true, &snapshots);
        check("the shadow-mask view snapshots at combine, the moment contact shadows run",
            snapshots == std::vector<Step> { Step::combine } && report.ran.bits == PHOTOREAL_PASS_CONTACT_SHADOWS);
        check("describe names the pass and the view", describe(report) == std::string(kAllSteps) + "; ran contact-shadows@combine; view shadow-mask");
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
        const auto glyphs = [](const Label &label) { return std::vector<int>(label.glyphs.begin(), label.glyphs.begin() + label.length); };
        check("the shadow-mask label spells SHADOW-MASK in font glyphs",
            glyphs(view_label(View::shadow_mask, true)) == std::vector<int> { 19, 8, 1, 4, 15, 23, 37, 13, 1, 19, 11 });
        check("a missing view's label adds a space and MISSING",
            glyphs(view_label(View::hdr_scene, false)) == std::vector<int> { 8, 4, 18, 37, 19, 3, 5, 14, 5, 0, 13, 9, 19, 19, 9, 14, 7 });
        check("digits, either case, and an unknown character as '?'", glyphs(encode_label("Ab09_")) == std::vector<int> { 1, 2, 27, 36, 38 });
        check("a label stops at 32 glyphs", encode_label(std::string(40, 'z')).length == 32);
    }
    {
        FrameReport report = FrameReport::start(parse_settings(PhotorealSettings { sizeof(PhotorealSettings), 0, 0, 1, {}, {} }));
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
    }
    {
        check("disabled settings want no step", wanted_steps(view_only(PHOTOREAL_VIEW_HDR_SCENE)).bits != 0
            && wanted_steps(parse_settings(PhotorealSettings { sizeof(PhotorealSettings), 0, PHOTOREAL_VIEW_HDR_SCENE, 1, { 1, 1, 1, 1, 1 }, { 1, 1, 1, 1, 1 } })).empty());
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
