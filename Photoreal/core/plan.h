// What we do with the frame map: settings, our passes and views as tables, the per-step plan, and the frame report.
// Pure C++17 like recipe.h. The GPU side lives in gpu.h and is chosen by PassId in addon.cpp.
#pragma once

#include "recipe.h"

#include <string>

struct PhotorealSettings;  // photoreal.h, read only by parse_settings

namespace photoreal
{
    enum class View : uint8_t
    {
        off, normals, albedo, specular, material_id, smoothness, character, depth, stencil,
        shadow_mask, quarter_shadow, ambient_diffuse, ambient_specular, hdr_scene, count,
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

    // The domain form of PhotorealSettings. Built only by parse_settings, so every value in it is in range.
    struct Settings
    {
        bool enabled = false;
        View view = View::off;
        bool flip = true;
        AmbientSettings ambient;
    };

    // Honors size (fields past it keep their defaults), maps an unknown view to off, replaces non-finite floats with the
    // default and clamps the rest. Never fails: the worst input is the default settings. raw must be a whole struct;
    // the shell copies the caller's bytes into one first.
    Settings parse_settings(const PhotorealSettings &raw);

    struct Camera
    {
        float world_to_view[16];
        float view_to_clip[16];
    };

    // Inverts a 4x4 matrix in either memory order. false when it is singular.
    bool invert(const float m[16], float out[16]);

    // What a pass asks of the game's call it runs before. The tonemap pass will skip the game's tonemap draw.
    enum class GameCall : bool { keep, skip };

    enum class PassId : uint8_t { ambient, count };
    using PassSet = Set<PassId>;

    struct PassSpec
    {
        PassId id;
        const char *name;
        Step at;                // runs when this step completes, before the game's call
        EntrySet needs;         // runs only when every one was found by then
        bool needs_camera;
        bool (*wanted)(const Settings &);
    };

    // Table order is run order within a step. Sun shadows will precede ambient at combine.
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

    struct Decision
    {
        enum class Kind : uint8_t { run, off, skip } kind;
        PassId pass;
        EntrySet missing;       // skip only
        bool camera_missing;    // skip only
    };

    // The passes subscribed to a step, in table order, each with what to do now.
    struct Plan
    {
        std::array<Decision, static_cast<size_t>(PassId::count)> items;
        uint8_t count = 0;
    };

    Plan plan(Step step, const FrameMap &map, const Settings &settings, bool camera_known);

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

        // A frame's report as it starts: armed and the view follow the settings, until the shell says otherwise.
        static FrameReport start(const Settings &settings);

        void note(const Plan &plan);
        // At each moment: true when the view snapshots now and its entry was found, so the shell copies it.
        bool note_view(Step step, const FrameMap &map);
        // A pass that was to run, but whose GPU side could not.
        void note_failed(PassId pass);
        // At present: a wanted pass whose step never came counts as skipped, missing its needs minus what was found.
        void finish(const FrameTracker &tracker, const Settings &settings, bool camera_known);

        View view_shown() const { return view_found ? view : View::off; }
    };

    // "1152x720 found gbuffer ... tonemap; ran ambient@combine; view normals", or
    // "found nothing; skipped ambient: missing normals depth ambient-diffuse". Names come from kRecipe, kEntryNames,
    // kPasses and kViews.
    std::string describe(const FrameReport &report);
}
