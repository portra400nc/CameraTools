// What we do with the frame map: settings, our passes and views as tables, the per-step plan, and the frame report.
// Pure C++17 like recipe.h. The GPU side lives in gpu.h and is chosen by PassId in addon.cpp.
#pragma once

#include "recipe.h"
#include "sun.h"

#include <string>
#include <string_view>
#include <vector>

struct EnhancementsSettings;  // enhancements.h, read only by parse_settings
struct EnhancementsCamera;    // enhancements.h, read only by parse_camera
struct EnhancementsAccumulate;  // enhancements.h, read only by parse_accumulate
struct EnhancementsShot;      // enhancements.h, read only by parse_shot

namespace enhancements
{
    enum class View : uint8_t
    {
        off, normals, albedo, specular, material_id, smoothness, character, depth, stencil,
        shadow_mask, quarter_shadow, ambient_diffuse, ambient_specular, hdr_scene, bloom_final, sun_atlas, light_shafts, trace, bounce, sky_visibility,
        sky_map, count,
    };

    // How the view shader turns an entry into a picture. One value per branch of shaders/view_ps.hlsl.
    enum class Decode : uint8_t { rgb, gray, material_palette, depth_reversed, stencil_palette, hdr, missing };

    struct AmbientSettings
    {
        bool enabled = false;
        float level = 0.6f;
        float ao_strength = 0.5f;
        float ao_radius = 1.0f;
        float foliage_ao_strength = 0.5f;
    };

    struct ContactShadowSettings
    {
        bool enabled = false;
        float length = 0.6f;
        float strength = 1.0f;
        float thickness = 0.25f;
        float foliage_strength = 0.0f;
    };

    struct AtmosphereSettings
    {
        bool enabled = false;
        float density = 0.000325f;
        float height_falloff = 0.02f;
        float sun_scatter = 1.0f;
        float anisotropy = 0.7f;
    };

    enum class Curve : uint8_t { game, agx, neutral, count };  // ENHANCEMENTS_CURVE_*, one branch each in shaders/tonemap_ps.hlsl

    extern const std::array<const char *, static_cast<size_t>(Curve::count)> kCurveNames;

    struct TonemapSettings
    {
        bool enabled = false;
        float exposure_ev = 0.0f;
        Curve curve = Curve::game;
        float bloom_strength = 1.0f;
        float saturation = 1.0f;
        float contrast = 1.0f;
    };

    struct SunShadowSettings
    {
        bool enabled = false;
        float light_size = 0.03f;
        float min_penumbra = 0.02f;
        float strength = 1.0f;
    };

    struct WetnessSettings
    {
        bool enabled = false;
        float wetness = 0.0f;   // 0: the weather's
        float darkening = 0.35f;
        float puddles = 0.3f;
    };

    struct LightShaftSettings
    {
        bool enabled = false;
        float strength = 3.0f;
        float density = 0.006f;
        float height = 5.0f;
        float anisotropy = 0.6f;
        float distance = 150.0f;
        float patchiness = 1.0f;
        float patch_size = 6.0f;
        float ambient = 0.3f;
        uint32_t steps = 48;
    };

    // EnhancementsTrace's. The traced occlusion is applied with ambient.level, so the traced and the raster ambient share one
    // level.
    struct TraceSettings
    {
        bool enabled = false;
        bool ao = true, sun = true;                 // EnhancementsTrace.outputs
        bool characters = true, alpha_cards = true; // EnhancementsTrace.casters
        bool whole_cards = false;                   // ENHANCEMENTS_TRACE_CAST_WHOLE_CARDS: the alpha tests off
        float ao_distance = 2.0f;
        float ao_strength = 1.0f;
        float sun_size = 0.0093f;
        float sun_strength = 1.0f;
        uint32_t ao_samples = 64;
        uint32_t sun_samples = 32;
        // Frames are long while the samples come in and no longer once they are in: build 209's Deck gave the kernels 18 ms
        // of its 69 ms frames at a budget of 20, and so spent three quarters of a converging shot's wait outside them.
        float budget_ms = 100.0f;
    };

    enum class GatherSky : uint8_t { occlusion, visibility, weighted, map, count };   // ENHANCEMENTS_GATHER_SKY_*
    extern const std::array<const char *, static_cast<size_t>(GatherSky::count)> kGatherSkyNames;

    // EnhancementsGather's.
    struct GatherSettings
    {
        bool enabled = false;
        // With the two filter passes, 64 samples leave build 209's all-faces frame 0.5/255 from 256 other samples in the
        // picture's RMS, against 0.4 for 128 (README, "Gathering").
        uint32_t samples = 64;
        GatherSky sky = GatherSky::visibility;
        uint32_t filter = 2;
        float strength = 1.0f;
        float hit_ambient = 0.5f;
    };

    // EnhancementsDepthOfField's.
    struct DepthOfFieldSettings
    {
        bool enabled = false;
        float f_number = 2.8f;
        float sensor_height_mm = 24;
        float focus_distance = 0;               // meters; 0: the autofocus at focus_point
        float focus_point[2] = { 0.5f, 0.5f };  // 0 to 1 from the top left
        float focus_speed = 0.9f;               // the share of the way the autofocus goes in a second; 0 jumps
        float max_blur = 0.03f;                 // the largest CoC radius over the render height
        uint32_t blades = 0;                    // 0 round, else 5 to 16
        float blade_roundness = 0.5f;
        float blade_rotation = 0;               // degrees
        float spherical_aberration = 0.6f;
        float cat_eye = 0;
        float swirl = 0;
        float anamorphic = 1;
        float chromatic = 0;
        uint32_t samples = 49;
        float highlight_threshold = 2;          // over the frame's mean luminance
        float highlight_gain = 2;               // 0: highlights keep their own light
        bool scatter = true;                    // highlights drawn as sprites
        float bokeh_contrast = 0;                // 0: sources keep their own light in the far field
    };

    // Every value in range: non-finite floats take the default, the rest clamp, and 1 to 4 blades read as 5.
    DepthOfFieldSettings clamp_depth_of_field(const DepthOfFieldSettings &settings);

    // The domain form of EnhancementsSettings. Built only by parse_settings, so every value in it is in range.
    struct Settings
    {
        bool enabled = false;
        View view = View::off;
        bool flip = true;
        AmbientSettings ambient;
        ContactShadowSettings contact_shadows;
        AtmosphereSettings atmosphere;
        TonemapSettings tonemap;
        SunShadowSettings sun_shadows;
        WetnessSettings wetness;
        LightShaftSettings light_shafts;
        TraceSettings trace;
        GatherSettings gather;
        DepthOfFieldSettings depth_of_field;
    };

    // Honors size (fields past it keep their defaults), maps an unknown view to off and an unknown curve to the game's,
    // replaces non-finite floats with the default and clamps the rest. Never fails: the worst input is the default
    // settings. raw must be a whole struct; the shell copies the caller's bytes into one first.
    Settings parse_settings(const EnhancementsSettings &raw);

    // The aperture point a frame was rendered from, and whether the accumulator adds the frame.
    struct LensSample
    {
        float x = 0, y = 0;     // in the unit disk, x right and y up
        uint32_t index = 0;
        bool mark = false;
    };

    struct Camera
    {
        float world_to_view[16];
        float view_to_clip[16];
        float toward_sun[3];    // unit length, or zero when there is no sun
        float sun_color[3];     // linear RGB times intensity, each at least 0
        float sky_color[3];     // linear RGB, each at least 0
        float wetness;          // the weather's, 0 dry to 1 soaked
        LensSample lens;
    };

    // Copies the matrices and normalizes the sun direction. A zero or non-finite direction becomes zero. A color
    // component that is not finite becomes 0, and a negative one 0 too. The weather's wetness clamps to 0..1, and one that
    // is not finite reads as dry. A lens coordinate that is not finite becomes 0 and the rest clamp to -1..1; an index that
    // is not a finite number from 0 to 2^32 becomes 0, and a mark that is not a finite nonzero number reads as unmarked.
    Camera parse_camera(const EnhancementsCamera &raw);

    enum class AccumulateMode : uint8_t { off, add, present, count };  // ENHANCEMENTS_ACCUMULATE_*

    // The domain form of EnhancementsAccumulate. Built only by parse_accumulate, so every value in it is in range.
    struct AccumulateSettings
    {
        AccumulateMode mode = AccumulateMode::off;
        uint32_t generation = 0;
        float cat_eye = 0.0f;
        float cat_eye_falloff = 0.1f;
    };

    // Honors size like parse_settings, maps an unknown mode to off, replaces non-finite floats with the default and
    // clamps the rest.
    AccumulateSettings parse_accumulate(const EnhancementsAccumulate &raw);

    // The domain form of EnhancementsShot. Built only by parse_shot, so every value in it is in range.
    struct ShotSettings
    {
        bool active = false;
        uint32_t samples = 0;       // 0: the applied gather's
        float budget_ms = 0;        // 0: the applied trace's
    };

    // Honors size like parse_settings. A null pointer is no shot. samples clamps to 4096 and budget_ms to 2..200; 0, and a
    // budget that is not finite, keep the applied one.
    ShotSettings parse_shot(const EnhancementsShot *raw);

    // The settings a frame runs with: the applied ones, and while a shot is active, with the trace and the gather on and
    // the shot's samples and budget where it gives them. Under a master switch that is off the shot runs alone: the
    // master switch on, every other pass and the view off, and the passes' values as applied, the ambient level among them.
    Settings with_shot(const Settings &applied, const ShotSettings &shot);

    // The wetness the wetness pass applies: the setting's while it is above 0, else the weather's from the camera.
    float wetness_amount(const WetnessSettings &settings, const Camera &camera);

    // shaders/wet.hlsli's formulas, compiled from the same source: how much puddle covers a pixel, how wet it is, and its
    // smoothness when wet.
    float puddle_cover(float noise, float up, float wetness, float puddles);
    float wet_share(float wetness, float up, float puddle);
    float wet_smoothness(float smoothness, float wet, float puddle);

    // A march through the light shafts' mist in one color channel, as shaders/scatter.hlsli's Mist holds it.
    struct Mist
    {
        float inscatter = 0;
        float transmittance = 1;
    };

    // shaders/scatter.hlsli's formulas, compiled from the same source, in one color channel: the Henyey-Greenstein phase,
    // the mist's extinction, where a march's samples lie and how long their spans are, a step's light, one step, the march
    // over the scene, and the march taken to a ray ratio times as long.
    float henyey_greenstein(float cosine, float g);
    float mist_density(float density, float rise, float height, float noise, float patchiness);
    float march_at(float i, float n, float jitter, float ray_length);
    float march_span(float i, float n, float ray_length);
    float mist_light(float sun, float visibility, float sky);
    Mist mist_step(Mist m, float density, float dt, float light);
    float mist_over(float scene, Mist m);
    Mist mist_rescale(Mist m, float ratio);

    // The light shafts' patch noise repeats every kNoiseSize lattice cells along each axis; shaders/shafts_ps.hlsl says the
    // same.
    constexpr uint32_t kNoiseSize = 64;

    // The noise's lattice: kNoiseSize^3 random bytes, x fastest, then y, then z, which the add-on uploads once as a 3D
    // texture that the march samples with wrapping. The same bytes every time.
    std::vector<uint8_t> noise_lattice();

    // Inverts a 4x4 matrix in either memory order. false when it is singular.
    bool invert(const float m[16], float out[16]);

    // What a pass asks of the game's call it runs before. The tonemap pass skips the game's tonemap draw.
    enum class GameCall : bool { keep, skip };

    // The ENHANCEMENTS_PASS_* bit order, not the run order.
    enum class PassId : uint8_t { ambient, contact_shadows, atmosphere, tonemap, sun_shadows, wetness, light_shafts, trace, depth_of_field, count };

    // ENHANCEMENTS_TRACE_STATE_*: where the traced scene the trace pass needs is. The names follow in kTraceStateNames.
    enum class TraceState : uint8_t { none, building, uploading, ready, stale, failed, count };
    extern const std::array<const char *, static_cast<size_t>(TraceState::count)> kTraceStateNames;
    using PassSet = Set<PassId>;

    // How a pass reads the game's sun shadow constants and atlas, which the shell copies at the shadow-mask draw.
    enum class SunUse : uint8_t
    {
        none,
        optional,   // when this frame's passed their checks; the pass does without them otherwise
        required,   // the pass skips without this frame's
    };

    struct PassSpec
    {
        PassId id;
        const char *name;
        Step at;                // runs when this step completes, before the game's call
        EntrySet needs;         // runs only when every one was found by then
        bool needs_camera;
        SunUse sun;
        bool (*wanted)(const Settings &);
    };

    // Table order is run order within a step: wetness at gbuffer-done; the trace, sun shadows, contact shadows, then ambient
    // at combine; atmosphere, light shafts, then depth of field at bloom; tonemap at tonemap.
    extern const std::array<PassSpec, static_cast<size_t>(PassId::count)> kPasses;

    struct ViewSpec
    {
        View id;
        const char *name;
        Entry entry;
        Step snapshot_at;       // copied after the passes at this step ran, so the view shows what the game reads next
        Decode decode;
        // A pass whose own result the view shows in place of the entry, found only when the pass ran; count for an entry.
        PassId from = PassId::count;
    };

    extern const std::array<ViewSpec, static_cast<size_t>(View::count)> kViews;  // kViews[off] is unused

    // The debug composite's label, as glyph indices into shaders/view_ps.hlsl's font: 0 space, 1-26 A-Z, 27-36 0-9,
    // 37 '-', 38 '?', 39 ':', 40 ','.
    struct Label
    {
        std::array<uint8_t, 32> glyphs {};
        uint8_t length = 0;
    };

    // Letters of either case share a glyph, and any other character reads as '?'. Text past the capacity is cut.
    Label encode_label(std::string_view text);

    // The view's name as describe() writes it; when its entry was not found, then ": " and why, or " missing" without one.
    Label view_label(View view, bool found, std::string_view why = {});

    struct Decision
    {
        // replaced: wanted, but the trace pass writes its output this frame, so it does not run.
        enum class Kind : uint8_t { run, off, skip, replaced } kind;
        PassId pass;
        EntrySet missing;       // skip only
        bool camera_missing;    // skip only
        SunCheck sun;           // skip only: not ok when the pass requires the sun constants and this frame's are unusable
        bool uses_sun;          // run only: the pass reads the sun constants, and this frame's passed their checks
        TraceState scene = TraceState::ready;   // skip only: the trace pass's scene, when it is not ready
    };

    // The passes subscribed to a step, in table order, each with what to do now.
    struct Plan
    {
        std::array<Decision, static_cast<size_t>(PassId::count)> items;
        uint8_t count = 0;
    };

    // What the frame has besides its map: a camera from EnhancementsSetCamera, the verdict on the sun constants, the traced
    // scene's state, and whether the trace pass writes the frame this frame: every traced pixel has a sample, and the
    // samples did not just start again.
    struct Inputs
    {
        bool camera = false;
        SunCheck sun = SunCheck::unread;
        TraceState scene = TraceState::none;
        bool traced = false;
    };

    // Whether the trace pass writes this pass's output when it writes the frame: sun shadows and contact shadows with its
    // sun visibility, ambient with its occlusion.
    bool replaced_by_trace(PassId pass, const TraceSettings &trace);

    Plan plan(Step step, const FrameMap &map, const Settings &settings, const Inputs &inputs);

    // Whether a wanted pass reads the game's sun shadow constants, so the shell copies them and the atlas this frame.
    bool wants_sun(const Settings &settings);

    // The steps a frame must reach for these settings: the steps of wanted passes, the view's snapshot step, and
    // their prerequisites.
    StepSet wanted_steps(const Settings &settings);

    // Why a wanted pass did not run.
    struct SkipReason
    {
        EntrySet missing;
        bool step_missed = false;   // its step never came, though every entry it needs was found
        bool camera_missing = false;
        bool failed = false;        // its inputs were there, but a texture of ours could not be made
        SunCheck sun = SunCheck::ok;
        TraceState scene = TraceState::ready;   // the trace pass's scene was not ready
    };

    // One frame's outcome, built by the shell as steps complete, published at present.
    struct FrameReport
    {
        bool armed = false;
        Size render;
        StepSet found;
        StepSet fell_back;      // steps whose moment came at their fallback's, which found leaves out
        StepSet wanted;
        PassSet ran;
        PassSet skipped;
        PassSet replaced;       // wanted, but the trace pass wrote their output
        uint32_t trace_ao = 0, trace_sun = 0;   // the samples each traced pixel has, once the trace pass ran
        uint32_t trace_gather = 0;              // the gather's, while it runs; then trace_ao is unused
        bool gather_without_light = false;      // the gather is on over a scene captured without ENHANCEMENTS_SCENE_LIGHT
        std::array<SkipReason, static_cast<size_t>(PassId::count)> why {};
        EntrySet missing;
        View view = View::off;  // the view this frame wanted
        bool view_found = false;
        uint32_t restarts = 0;
        uint32_t error = 0;     // ENHANCEMENTS_ERROR_*
        uint32_t compare_saved = 0, compare_count = 0;  // a comparison capture's progress; count 0 when none runs
        uint32_t scene_saved = 0, scene_count = 0;      // a scene capture's faces written and requested; count 0 when none runs
        std::string scene_stopped;                      // why the last scene capture stopped, until the next one starts
        std::string frame_saved;                        // the folder EnhancementsSaveFrame last saved into
        AccumulateMode accumulate = AccumulateMode::off;
        uint32_t accumulated = 0;   // the sum's frames, which present mode reports

        // A frame's report as it starts: armed and the view follow the settings, until the shell says otherwise.
        static FrameReport start(const Settings &settings);

        void note(const Plan &plan);
        // At each moment: true when the view snapshots now and its entry was found, so the shell copies it.
        bool note_view(Step step, const FrameMap &map);
        // A pass that was to run, but whose GPU side could not.
        void note_failed(PassId pass);
        // At present: a wanted pass whose step never came counts as skipped, missing its needs minus what was found.
        void finish(const FrameTracker &tracker, const Settings &settings, const Inputs &inputs);

        View view_shown() const { return view_found ? view : View::off; }
        // The steps whose moment came, matched or at their fallback's.
        StepSet moments() const { StepSet s = found; s |= fell_back; return s; }
    };

    // Why a wanted pass did not run, as describe() writes it after "skipped <pass>: ", such as "scene stale",
    // "missing normals depth" or "no camera, scene building".
    std::string skip_text(const SkipReason &why, PassId pass);

    // Why the report's view, drawn by a pass of ours, has nothing to show: its pass's skip_text, or "off" when the pass
    // was not wanted. Empty while it was found, for a view of a game entry, and when its pass ran but the copy failed.
    std::string view_missing_why(const FrameReport &report);

    // "1152x720 found gbuffer ... tonemap; ran ambient@combine; view normals", or "...; ran trace@combine (64 ao, 32 sun);
    // replaced sun-shadows contact-shadows ambient" once the trace pass writes the frame, "(128 gather, 32 sun)" while it
    // gathers, and "; skipped gather: scene without light" while the gather is on over a scene without light, where a pass whose step fell back names
    // the fallback ("found ... combine tonemap ...; ran light-shafts@tonemap"), or
    // "found nothing; skipped ambient: missing normals depth ambient-diffuse", or "...; skipped trace: scene building", or
    // "off; comparing 2/5", or
    // "1152x720 found ...; presenting 96 samples", or "...; scene 3/7" while a scene capture runs and
    // "...; scene stopped: <reason>" after one stopped, and "...; saved frame-<time>" after a frame save. Accumulating names no
    // count, so the line does not change every frame.
    // Names come from kRecipe, kEntryNames, kPasses and kViews.
    std::string describe(const FrameReport &report);
}
