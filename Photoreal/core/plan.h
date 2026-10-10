// What we do with the frame map: settings, our passes and views as tables, the per-step plan, and the frame report.
// Pure C++17 like recipe.h. The GPU side lives in gpu.h and is chosen by PassId in addon.cpp.
#pragma once

#include "recipe.h"
#include "sun.h"

#include <string>
#include <string_view>

struct PhotorealSettings;  // photoreal.h, read only by parse_settings
struct PhotorealCamera;    // photoreal.h, read only by parse_camera
struct PhotorealAccumulate;  // photoreal.h, read only by parse_accumulate

namespace photoreal
{
    enum class View : uint8_t
    {
        off, normals, albedo, specular, material_id, smoothness, character, depth, stencil,
        shadow_mask, quarter_shadow, ambient_diffuse, ambient_specular, hdr_scene, bloom_final, sun_atlas, count,
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

    enum class Curve : uint8_t { game, agx, neutral, count };  // PHOTOREAL_CURVE_*, one branch each in shaders/tonemap_ps.hlsl

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

    // The domain form of PhotorealSettings. Built only by parse_settings, so every value in it is in range.
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
    };

    // Honors size (fields past it keep their defaults), maps an unknown view to off and an unknown curve to the game's,
    // replaces non-finite floats with the default and clamps the rest. Never fails: the worst input is the default
    // settings. raw must be a whole struct; the shell copies the caller's bytes into one first.
    Settings parse_settings(const PhotorealSettings &raw);

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
        LensSample lens;
    };

    // Copies the matrices and normalizes the sun direction. A zero or non-finite direction becomes zero. A color
    // component that is not finite becomes 0, and a negative one 0 too. A lens coordinate that is not finite becomes 0
    // and the rest clamp to -1..1; an index that is not a finite number from 0 to 2^32 becomes 0, and a mark that is not a
    // finite nonzero number reads as unmarked.
    Camera parse_camera(const PhotorealCamera &raw);

    enum class AccumulateMode : uint8_t { off, add, present, count };  // PHOTOREAL_ACCUMULATE_*

    // The domain form of PhotorealAccumulate. Built only by parse_accumulate, so every value in it is in range.
    struct AccumulateSettings
    {
        AccumulateMode mode = AccumulateMode::off;
        uint32_t generation = 0;
        float cat_eye = 0.0f;
        float cat_eye_falloff = 0.1f;
    };

    // Honors size like parse_settings, maps an unknown mode to off, replaces non-finite floats with the default and
    // clamps the rest.
    AccumulateSettings parse_accumulate(const PhotorealAccumulate &raw);

    // Inverts a 4x4 matrix in either memory order. false when it is singular.
    bool invert(const float m[16], float out[16]);

    // What a pass asks of the game's call it runs before. The tonemap pass skips the game's tonemap draw.
    enum class GameCall : bool { keep, skip };

    // The PHOTOREAL_PASS_* bit order, not the run order.
    enum class PassId : uint8_t { ambient, contact_shadows, atmosphere, tonemap, sun_shadows, count };
    using PassSet = Set<PassId>;

    struct PassSpec
    {
        PassId id;
        const char *name;
        Step at;                // runs when this step completes, before the game's call
        EntrySet needs;         // runs only when every one was found by then
        bool needs_camera;
        bool needs_sun;         // the game's sun shadow constants, checked this frame
        bool (*wanted)(const Settings &);
    };

    // Table order is run order within a step: sun shadows, contact shadows, then ambient at combine; atmosphere at bloom;
    // tonemap at tonemap.
    extern const std::array<PassSpec, static_cast<size_t>(PassId::count)> kPasses;

    struct ViewSpec
    {
        View id;
        const char *name;
        Entry entry;
        Step snapshot_at;       // copied after the passes at this step ran, so the view shows what the game reads next
        Decode decode;
    };

    extern const std::array<ViewSpec, static_cast<size_t>(View::count)> kViews;  // kViews[off] is unused

    // The debug composite's label, as glyph indices into shaders/view_ps.hlsl's font: 0 space, 1-26 A-Z, 27-36 0-9,
    // 37 '-', 38 '?'.
    struct Label
    {
        std::array<uint8_t, 32> glyphs {};
        uint8_t length = 0;
    };

    // Letters of either case share a glyph, and any other character reads as '?'. Text past the capacity is cut.
    Label encode_label(std::string_view text);

    // The view's name as describe() writes it, then " missing" when its entry was not found.
    Label view_label(View view, bool found);

    struct Decision
    {
        enum class Kind : uint8_t { run, off, skip } kind;
        PassId pass;
        EntrySet missing;       // skip only
        bool camera_missing;    // skip only
        SunCheck sun;           // skip only: not ok when the pass needs the sun constants and this frame's are unusable
    };

    // The passes subscribed to a step, in table order, each with what to do now.
    struct Plan
    {
        std::array<Decision, static_cast<size_t>(PassId::count)> items;
        uint8_t count = 0;
    };

    // What the frame has besides its map: a camera from PhotorealSetCamera, and the verdict on the sun constants.
    struct Inputs
    {
        bool camera = false;
        SunCheck sun = SunCheck::unread;
    };

    Plan plan(Step step, const FrameMap &map, const Settings &settings, const Inputs &inputs);

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
    };

    // One frame's outcome, built by the shell as steps complete, published at present.
    struct FrameReport
    {
        bool armed = false;
        Size render;
        StepSet found;
        StepSet wanted;
        PassSet ran;
        PassSet skipped;
        std::array<SkipReason, static_cast<size_t>(PassId::count)> why {};
        EntrySet missing;
        View view = View::off;  // the view this frame wanted
        bool view_found = false;
        uint32_t restarts = 0;
        uint32_t error = 0;     // PHOTOREAL_ERROR_*
        uint32_t compare_saved = 0, compare_count = 0;  // a comparison capture's progress; count 0 when none runs
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
    };

    // "1152x720 found gbuffer ... tonemap; ran ambient@combine; view normals", or
    // "found nothing; skipped ambient: missing normals depth ambient-diffuse", or "off; comparing 2/5", or
    // "1152x720 found ...; presenting 96 samples". Accumulating names no count, so the line does not change every frame.
    // Names come from kRecipe, kEntryNames, kPasses and kViews.
    std::string describe(const FrameReport &report);
}
