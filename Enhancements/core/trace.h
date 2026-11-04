// The trace pass's decisions: its constants as the compute shader reads them, the tiles and the order they are traced
// in, how many each frame gets within the time budget, when the samples start again, the game's sun direction and
// floating origin from its shadow constants, when the scene has gone stale, and the per-pixel kernel itself, which is
// shaders/trace.hlsli compiled as C++ (core/kernel.cpp). Pure C++17 like plan.h. The GPU side lives in trace_gpu.*.
#pragma once

#include "bvh.h"
#include "light.h"
#include "plan.h"
#include "sun.h"

#include <array>
#include <optional>
#include <string>
#include <vector>

struct EnhancementsShotLight;  // enhancements.h, read only by parse_trace_lights

namespace enhancements
{
    // The classes a ray sees with these settings, as bits 1 << TraceClass: the world and other classes always,
    // vegetation and foliage with alpha cards, characters with characters; and kAlphaTests unless whole cards.
    uint32_t caster_classes(const TraceSettings &settings);

    // shaders/trace_cs.hlsl's cbuffer Trace at b0, which apply_trace_ps.hlsl reads at b1: 160 bytes.
    struct TraceConstants
    {
        float clip_rows[4][4];      // D3D clip (ndc x, ndc y, reversed depth, 1) to the scene's frame, homogeneous, by rows
        float eye_position[4];      // xyz: the camera in the scene's frame; w: how far from the reference the scene holds every
                                    // triangle, past which the consistency grid tests nothing; 0 for all of it
        float sun_cone[4];          // xyz: toward the sun, unit length; w: the cosine of the cone's half angle; all 0 for no sun
        float trace_params[4];      // ao_distance, ao_strength, sun_strength, ambient level
        uint32_t trace_counts[4];   // ao_samples, sun_samples, caster classes, outputs (1 occlusion, 2 sun)
        uint32_t trace_tiles[4];    // this dispatch's first tile in tile_order, the frame's tile count, 0, the seed
        uint32_t trace_render[4];   // width, height, 1 when game targets are stored upside down, the consistency grid's stride
    };
    static_assert(sizeof(TraceConstants) == 160);

    // The camera's view of the scene for these settings: clip_rows from the pushed camera's inverse projection and view,
    // in double, with the reference subtracted, and the cone of toward_sun (zero: no sun). Empty when a matrix does not
    // invert. The tiles fields are left 0, for each dispatch to set.
    std::optional<TraceConstants> trace_constants(const TraceSettings &settings, float level, const Camera &camera, bool flip, Size render,
        const Vec3d &reference, const Vec3 &toward_sun, uint32_t seed);

    // The camera's position in the world, from its world_to_view, in double.
    Vec3d camera_position(const Camera &camera);

    // A traced light, parsed from EnhancementsShotLight: every value finite and in range.
    struct TraceLight
    {
        Vec3d position {};
        float range = 1;
        Vec3 radiance {};           // color times intensity, to the power kLocalLightPower
        float cos_outer = -2;       // below -1: a point light
        Vec3 direction { 0, 0, 1 };
        float cos_inner = -2;
        bool shadows = false;       // the game's own light draws with shadows: bake_game_lights gives it shadow rays
        bool operator==(const TraceLight &o) const;
    };

    // Unity's lights take their color times intensity from gamma to linear.
    constexpr float kLocalLightPower = 2.2f;

    // The lights the caller gave, the first max, without those of no range or no light, and with
    // a spot's direction unit length and its inner cosine at least its outer.
    std::vector<TraceLight> parse_trace_lights(const ::EnhancementsShotLight *lights, uint32_t count, uint32_t max = 4);

    // The game's own lights, baked once into the traced scene for the gather's hits that no face shows: each triangle's
    // pad1 holds the light on its front, its albedo times the sum of what reaches it, with a shadow ray only for the lights
    // the game itself shadows: most of its lights have none, and the faces show that light passing through walls. A hit takes it without any ray of its own, so any number of lights adds no noise and no cost a sample. The
    // word is 10 bits a channel, each the square root of the value over kBakedLightRange, as shaders/gather.hlsli reads it.
    constexpr float kBakedLightRange = 16.0f;
    uint32_t pack_baked_light(const Vec3 &radiance);
    // How far short of a light its shadow rays stop: the game's lamps sit inside their own models.
    constexpr float kGameLightClearance = 0.5f;
    struct GameLightCounts
    {
        uint64_t lit = 0;           // triangles with any of the lights' light
        uint64_t rays = 0;          // shadow rays
    };
    // The light's geometry at p of normal n, as shaders/local_light.hlsli's local_geometry_of: what multiplies its radiance.
    float light_geometry(const TraceLight &light, const Vec3d &p, const Vec3d &n);
    // Writes every triangle's pad1. The BVH's hash stays that of the scene without them.
    GameLightCounts bake_game_lights(Bvh &bvh, const std::vector<TraceLight> &lights, const Vec3d &reference, const std::vector<uint32_t> *alpha, uint32_t tasks);

    // The cbuffer GatherLights at b3, which both kernels and the traced lights' pass read: per light its position in the
    // scene's frame and range, its radiance and cos_outer, its direction and cos_inner, and in the last row's y the
    // radiance's luminance; then the count. 272 bytes.
    struct GatherLights
    {
        float rows[4][4][4];
        uint32_t info[4];           // the count, 0, 0, 0
    };
    static_assert(sizeof(GatherLights) == 272);

    GatherLights gather_lights(const std::vector<TraceLight> &lights, const Vec3d &reference);

    // shaders/local_light.hlsli's local_light_of: traced light k's light at p of normal n, before shadows, in the scene's frame.
    Vec3 kernel_local_light(const GatherLights &lights, uint32_t k, const Vec3 &p, const Vec3 &n);

    // shaders/gather_cs.hlsl's cbuffer Gather at b1, which apply_gather_ps.hlsl reads too: 112 bytes.
    struct GatherConstants
    {
        float sun[4];               // rgb: the sun's color, what a white surface facing it shows
        float shade[4];             // xyz: toward the sun as the game shades with it, unit length
        float ambient[4][4];        // rgb: the ambient light at a hit of normal n, rows[0] + rows[1] n.x + rows[2] n.y + rows[3] n.z,
                                    // the game's times hit_ambient. rows[0]'s a: how many of these the faces' pictures hold;
                                    // rows[1]'s a: how many times more light a hit surface reflects, strength above 1
        uint32_t counts[4];         // samples a pixel, the bounces, then 0, 0
    };
    static_assert(sizeof(GatherConstants) == 112);

    // The gather's constants: the sun's color and shading direction, zero with no sun; the ambient fit's rows times
    // hit_ambient; the samples and the bounces. faces_ambient is the share of the game's ambient light the faces' pictures hold,
    // ambient.level, which the traced and the raster ambient share.
    GatherConstants gather_constants(const GatherSettings &settings, const SunLight &sun, const AmbientFit &ambient, float faces_ambient = 0);

    // shaders/gather_cs.hlsl's cbuffer GatherFaces at b2: the cameras of core/light.h's FaceLights, which the gather's hits
    // take the game's light from, 576 bytes.
    struct GatherFaces
    {
        float rows[kFaceCount][4][4];   // each face's scene_to_clip
        float w[kFaceCount][4];         // each face's depth_to_w
        uint32_t info[4];               // the faces, their width and their height, 0
    };
    static_assert(sizeof(GatherFaces) == 576);

    // The first kFaceCount faces' cameras; all zero for none.
    GatherFaces gather_faces(const std::vector<FaceLight> &faces);

    // shaders/gather.cbuffer.hlsli's GatherApply at b2, which the gather's draws read: 176 bytes.
    struct GatherApplyConstants
    {
        float apply[4];             // strength, the filter's step in pixels, 0, 0
        uint32_t modes[4];          // the sky mode, the view (GatherView), 1 while the pixels have under 16 samples, 0
        float harmonics[9][4];      // rgb: the sky map's harmonics
    };
    static_assert(sizeof(GatherApplyConstants) == 176);

    // What the gather's first draw writes for a view: shaders/gather.cbuffer.hlsli's kGatherView*.
    enum class GatherView : uint8_t { none, bounce, sky_visibility, sky_map };
    GatherView gather_view(View view);

    // Below this many samples a pixel's gathered light is filtered at least once, as T1's apply draw blurs below 16.
    constexpr uint32_t kGatherBlurBelow = 16;
    // The filter's passes for the pixels' samples: the setting's, at least 1 below kGatherBlurBelow.
    uint32_t gather_filter_passes(const GatherSettings &settings, uint32_t samples);

    GatherApplyConstants gather_apply_constants(const GatherSettings &settings, const std::array<Vec3f, 9> &harmonics, GatherView view, uint32_t samples,
        uint32_t step);

    constexpr uint32_t kTileSize = 64;      // pixels a side; 64 groups of 8x8 threads
    constexpr uint32_t kGridStride = 16;    // every 16th pixel each way shoots a ray from the camera to check the scene
    constexpr uint32_t kStartBatch = 1;     // tiles of the first frame, whose measured time sets the next ones'
    // A GPU submission of the trace aims at this much time, so even a slow scene never holds the GPU long at once. A tile's
    // 4,096 pixels shoot at most 3 rays each, and a ray at its caps of shaders/bvh.hlsli costs about 16 us of the Deck's GPU
    // at worst (README, "Tracing"), so a lone tile takes at most about 0.2 s, well inside the GPU driver's timeout.
    constexpr double kSubmissionMs = 50;
    constexpr uint32_t kMaxChunkTiles = 64; // tiles of one submission at most, however cheap they measure

    // The render size's tiles in Morton order, each x | y << 16, so a frame's tiles lie together.
    std::vector<uint32_t> tile_order(Size render);

    // How far the samples have come: every frame traces the next tiles of tile_order from where the last frame stopped,
    // wrapping, so after visits tile visits every tile has had visits / tiles of them, and the first visits % tiles one more.
    struct TraceSchedule
    {
        uint32_t tiles = 0;
        uint64_t visits = 0;
        uint32_t batch = kStartBatch;   // tiles a frame, from the time budget
        uint32_t chunk = 1;             // tiles a GPU submission, from kSubmissionMs; 1 until a tile's time is measured
    };

    // Full passes: samples every traced pixel has.
    inline uint32_t trace_passes(const TraceSchedule &s) { return s.tiles == 0 ? 0 : static_cast<uint32_t>(s.visits / s.tiles); }

    struct TraceDispatch
    {
        uint32_t first = 0, count = 0;  // into tile_order, wrapping; count is at most the tiles, so no tile is traced twice
    };

    // This frame's tiles, and the schedule after them; empty once every pixel has samples samples (the larger of the
    // occlusion and sun samples).
    std::optional<TraceDispatch> trace_next(TraceSchedule &schedule, uint32_t samples);

    // A frame's tiles as the dispatches that trace them, each at most chunk tiles from where the last stopped, wrapping at
    // tiles.
    std::vector<TraceDispatch> trace_chunks(const TraceDispatch &frame, uint32_t tiles, uint32_t chunk);

    // The next frame's tiles, from a frame of batch tiles whose first measured tiles took measured_ms on the GPU:
    // measured * budget_ms / measured_ms, at most twice batch, so a cheap frame of sky tiles cannot make the next one long,
    // and from 1 to all tiles.
    uint32_t next_batch(uint32_t batch, uint32_t measured, double measured_ms, double budget_ms, uint32_t tiles);

    // Tiles of one GPU submission from measured tiles that took measured_ms: kSubmissionMs worth, from 1 to kMaxChunkTiles.
    uint32_t next_chunk(uint32_t measured, double measured_ms);

    // What the samples depend on: a change to any of it starts them again.
    // The cbuffer Reflect at b4, shaders/reflect.hlsli's reflect_params: what scales roughness, how much brighter light sources
    // get. All zeros while the shot keeps the game's reflections.
    struct ReflectConstants
    {
        float params[4] = {};
    };
    ReflectConstants reflect_constants(const ReflectSettings &settings);
    struct TraceKey
    {
        uint64_t generation = 0;    // the scene's
        Size render;
        std::array<float, 16> world_to_view {}, view_to_clip {};
        Vec3 toward_sun {};
        float ao_distance = 0, sun_size = 0;
        uint32_t casters = 0;
        // With the gather on: its sun color and shading direction, and its ambient rows times hit_ambient; all 0 while off.
        bool gather = false;
        std::array<float, 20> light {};
        uint32_t bounces = 0;
        float faces_ambient = 0;
        float surface = 0;
        GatherLights lights {};     // the traced lights; any change starts the samples again
        ReflectConstants reflect;   // and the reflections'
    };
    // gather is null while the gather is off, or its scene has no light.
    TraceKey trace_key(uint64_t generation, Size render, const Camera &camera, const Vec3 &toward_sun, const TraceSettings &settings,
        const GatherConstants *gather = nullptr, const GatherLights &lights = {}, const ReflectConstants &reflect = {});

    // Why a frame's samples start again, ENHANCEMENTS_TRACE_RESTART_*: the first of these that holds. A new scene, which a
    // scene's first samples are too; another render size; a setting the rays depend on, which is the ao distance, the sun
    // size, the casters, the gather switched on or off, or the hit's share of the ambient light, which scales the
    // gather's ambient rows; a camera matrix element differing by more than 1e-6 of the matrix's largest; the direction
    // toward the sun, the shadows' or the gather's shading direction, differing by more than 1e-5; or the gather's sun
    // color differing by more than 1e-5, or any change of the traced lights. before is the key the samples started with,
    // not the last frame's, so a slow drift starts them again once it has gone that far.
    enum class TraceRestart : uint8_t { none, scene, render, settings, camera, sun_direction, light, count };
    extern const std::array<const char *, static_cast<size_t>(TraceRestart::count)> kTraceRestartNames;
    TraceRestart trace_restart(const TraceKey &before, const TraceKey &now);
    inline bool trace_reset(const TraceKey &before, const TraceKey &now) { return trace_restart(before, now) != TraceRestart::none; }

    // What changed, for the log, with its values: "the sun's color went from (0.303, 0.808, 1.331) to (0.301, 0.805,
    // 1.327)". before is null for a scene's first samples.
    std::string restart_text(TraceRestart reason, const TraceKey *before, const TraceKey &now);

    // The starts since a full pass last completed, a burst, and the reasons the log has named in it.
    struct RestartBurst
    {
        uint32_t count = 0;
        uint32_t named = 0;     // bits 1 << TraceRestart
    };
    // Counts a start into the burst and says whether the log names it: each reason's first of the burst, and every
    // start whose count is a power of two from kRestartLogFrom on, so a light that changes every frame writes a line a
    // reason and then a few, not one a frame.
    constexpr uint32_t kRestartLogFrom = 32;
    bool restart_logged(RestartBurst &burst, TraceRestart reason);

    // The samples of one start share one camera, sun and light, the start's. A later frame's own may differ from them by
    // less than what starts the samples again, and following those from frame to frame would aim and light each pass a
    // little apart from the last, with no bound on where they end. So a frame that goes on takes from the start's
    // constants what the key holds, the clip rows, the eye, the sun's cone, the occlusion distance and the classes, and
    // keeps its own of the rest, which starts nothing: the strengths, the level, the samples, the outputs and the size.
    TraceConstants held_constants(const TraceConstants &start, const TraceConstants &now);
    // The gather's: the start's sun, shading direction and ambient rows, and now's samples.
    GatherConstants held_gather(const GatherConstants &start, const GatherConstants &now);

    // The samples every traced pixel gets: with the gather, the larger of its samples and the sun's, else the larger of
    // the occlusion's and the sun's.
    uint32_t trace_target(const TraceSettings &trace, const GatherSettings &gather, bool gathering);

    // Whether a frame whose tiles start at pass passes_before dispatches the trace kernel beside the gather kernel: while
    // the sun has samples to take, then every kTraceGridPasses passes for the consistency grid alone.
    constexpr uint32_t kTraceGridPasses = 8;
    bool trace_kernel_runs(uint32_t passes_before, uint32_t sun_samples);

    // What the trace reads from the game's shadow-mask constants without parse_sun_shadows' checks, which refused the
    // Deck's cascades in build 198: the direction toward the sun, cascade 0's depth gradient normalized (b3 rows 15 to
    // 17), and the floating origin, b1 row 10. Each empty when not finite, the gradient also when zero.
    struct SunFacts
    {
        std::optional<Vec3> toward_sun;
        std::optional<Vec3> floating_origin;
    };
    SunFacts sun_facts(const SunCbuffers &bytes);

    // A world shift: the floating origin moved by more than 1 cm.
    bool origin_moved(const Vec3 &before, const Vec3 &now);

    // The consistency grid: of its rays from the camera to world pixels that are not grass, vegetation or foliage, more
    // than 5% met the scene off the depth buffer by more than max(0.05 m, 1%), or not at all, out of at least 32.
    bool consistency_stale(uint64_t tested, uint64_t off);

    // During a traced shot the game is paused and the camera held, so the world cannot move: the grid then shows only what
    // the capture missed, which costs the shot little, and the scene goes stale only past half of its rays off.
    bool shot_consistency_stale(uint64_t tested, uint64_t off);

    // What the compute shader adds into its counters buffer (u1), in this order: rays, guard stops, stack overflows, grid
    // rays and those off, then for each GridClass its grid rays and those off by GridSlot.
    enum class TraceCounter : uint8_t { rays, guard, overflow, tested, off, grid };
    // The consistency grid's pixels by stencil: 128, 144, and any other world stencil that is not grass, vegetation or foliage.
    enum class GridClass : uint8_t { s128, s144, other, count };
    // A grid class's rays, and why those off are: the ray met nothing, met the scene nearer than the depth buffer, or farther.
    enum class GridSlot : uint8_t { tested, no_hit, nearer, farther, count };
    constexpr uint32_t kGridSlots = static_cast<uint32_t>(GridSlot::count);
    constexpr uint32_t kTraceCounters = static_cast<uint32_t>(TraceCounter::grid) + static_cast<uint32_t>(GridClass::count) * kGridSlots;
    constexpr uint32_t grid_counter(GridClass c, GridSlot s)
    {
        return static_cast<uint32_t>(TraceCounter::grid) + static_cast<uint32_t>(c) * kGridSlots + static_cast<uint32_t>(s);
    }
    // What the gather kernel adds into the same buffer after them, in this order: rays, guard stops, stack overflows, hits
    // by core/light.h's AlbedoSource and hits a face's lit picture showed, and the sums of each sample's distance from its
    // pixel's mean and of that mean, in luminance, in 1 / kNoiseUnits.
    enum class GatherCounter : uint8_t { rays, guard, overflow, direct, covered, material, class_mean, face, deviation, mean, count };
    constexpr uint32_t kGatherCounters = static_cast<uint32_t>(GatherCounter::count);
    constexpr uint32_t kAllCounters = kTraceCounters + kGatherCounters;
    constexpr uint32_t gather_counter(GatherCounter c) { return kTraceCounters + static_cast<uint32_t>(c); }
    // The noise sums in the buffer count in 1 / kNoiseUnits; the kernel's per-pixel values in 1 / 4096.
    constexpr double kNoiseUnits = 16;
    using TraceCounts = std::array<uint64_t, kAllCounters>;

    // The mean relative standard error of the gathered light, from a full pass's noise sums: 1.25 times the deviations'
    // sum over the means' sum, over the square root of the samples (mean absolute deviation is 0.8 standard deviations
    // for normal noise). 0 when the means are.
    double gather_error(uint64_t deviation, uint64_t mean, uint32_t samples);

    // The grid's rays by class and reason for the log: "stencil 128 0 of 4061 off, stencil 144 2025 of 2316 off (0 met
    // nothing, 0 nearer, 2025 farther), other 0 of 0 off", the reasons only where some are off.
    std::string grid_breakdown(const TraceCounts &counts);

    // The frame the kernel traces from: the stored main depth (reversed), the G-buffer normals as stored
    // (R10G10B10A2_UNORM, world normal * 0.5 + 0.5) and the stencil, each top row first.
    struct TraceImage
    {
        Size size;
        std::vector<float> depth;
        std::vector<uint32_t> normals;
        std::vector<uint8_t> stencil;
        std::vector<uint8_t> smoothness, material;  // for reflect_pixel: G-buffer rt4 and rt3 as stored
    };

    // One pixel's work in one dispatch, as the compute shader does it: on a world pixel with depth, one occlusion ray when
    // it has fewer than ao_samples, one sun ray when it has fewer than sun_samples, each the next of its own sequence, and
    // on the consistency grid one ray from the camera. history is (occlusion sum, sun sum, occlusion count, sun count). The
    // alpha tests run with alpha's buffer when the constants' classes ask for them; without one every triangle is whole.
    struct PixelTrace
    {
        std::array<float, 4> history {};
        std::array<float, 4> lights {};     // each traced light's visibility sum
        bool world = false;
        std::array<uint32_t, kTraceCounters> counters {};
    };
    PixelTrace trace_pixel(const Bvh &bvh, const TraceImage &image, const TraceConstants &constants, uint32_t x, uint32_t y,
        const std::array<float, 4> &history, const std::vector<uint32_t> *alpha = nullptr, const GatherLights *lights = nullptr,
        const std::array<float, 4> &light_sums = {});

    // shaders/trace.hlsli's ray generator, compiled as C++: the pixel's seed, the index-th point of its R2 sequence for
    // salt (0 occlusion, 1 sun), a cosine-weighted direction around a unit normal, and a direction in the cone around a unit
    // axis whose half angle has cosine cos_max.
    uint32_t pixel_seed(uint32_t x, uint32_t y, uint32_t generation);
    std::array<float, 2> sequence_point(uint32_t seed, uint32_t salt, uint32_t index);

    // One pixel's work in one gather dispatch, as shaders/gather_cs.hlsl does it: on a world pixel with fewer than
    // gather.counts[0] samples, the next sample, added to history's occlusion sum and count and to hit or sky, the sums of
    // shaders/gather_cs.hlsl's two buffers. sky is core/light.h's SkyMap::texels, and faces its SceneLight::faces, whose
    // pictures light the hits they show; none lights none. The sample's own light and the counters the kernel's group
    // adds are in sample and counters, the noise sums in 1 / 4096.
    struct GatherPixel
    {
        std::array<float, 4> history {}, hit {}, sky {};
        bool world = false;
        std::array<float, 4> light {};  // the sample's light, w 1 for a hit and 2 for the sky
        float occlusion = 1;            // its occlusion sample
        bool lit = false;               // the sun reached its hit
        std::array<uint32_t, kGatherCounters> counters {};
    };
    GatherPixel gather_pixel(const Bvh &bvh, const TraceImage &image, const TraceConstants &constants, const GatherConstants &gather,
        const std::vector<float> &sky, uint32_t x, uint32_t y, const std::array<float, 4> &history, const std::array<float, 4> &hit,
        const std::array<float, 4> &sky_sums, const std::vector<uint32_t> *alpha = nullptr, const std::vector<FaceLight> *faces = nullptr,
        const GatherLights *lights = nullptr);

    // How much a light source must outshine the view's mean luminance before it counts as one: windows and lanterns at the
    // indoor test spot reach 6 to 20 times its mean, and sunlit snow by day 1.5 times.
    constexpr float kSourceOverMean = 6;

    // One sample of a pixel's reflection, as the reflection kernel takes it; sum is its light summed and its samples, and
    // comes back with this sample added. world is false for a pixel that keeps the game's reflections.
    struct ReflectPixel
    {
        std::array<float, 4> sum {};
        bool world = false;
        uint32_t rays = 0, guard = 0;
    };
    ReflectPixel reflect_pixel(const Bvh &bvh, const TraceImage &image, const TraceConstants &constants, const GatherConstants &gather,
        const ReflectConstants &reflect, const std::vector<float> &sky, uint32_t x, uint32_t y, const std::array<float, 4> &sum,
        const std::vector<uint32_t> *alpha = nullptr, const std::vector<FaceLight> *faces = nullptr, const GatherLights *lights = nullptr);

    // shaders/gather.hlsli's sky_texel_of: the sky map's texel a direction reads.
    uint32_t sky_texel(const Vec3 &direction);
    // Its albedo_of: a light word's albedo, as the kernel reads it.
    Vec3 kernel_albedo(uint32_t light);
    // shaders/gather.hlsli's source_gain_of: how much brighter a light source of that luminance gets.
    float kernel_source_gain(float seen, float gain, float above);
    // shaders/reflect_common.hlsli's reflect_roughness_of.
    float kernel_reflect_roughness(float smoothness, float keep);

    // Its hdr_rgb_of: an R11G11B10_FLOAT value as the kernel reads a face's picture.
    Vec3 kernel_hdr_rgb(uint32_t stored);
    // Its face_light_of: the game's light at q, in the scene's frame, from the first of faces that shows it; empty when none does.
    std::optional<Vec3> kernel_face_light(const std::vector<FaceLight> &faces, const Vec3 &q);
    Vec3 cosine_direction(const Vec3 &normal, const std::array<float, 2> &u);
    Vec3 cone_direction(const Vec3 &axis, float cos_max, const std::array<float, 2> &u);
}
