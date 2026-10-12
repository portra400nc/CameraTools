// The trace pass's decisions: its constants as the compute shader reads them, the tiles and the order they are traced
// in, how many each frame gets within the time budget, when the samples start again, the game's sun direction and
// floating origin from its shadow constants, when the scene has gone stale, and the per-pixel kernel itself, which is
// shaders/trace.hlsli compiled as C++ (core/kernel.cpp). Pure C++17 like plan.h. The GPU side lives in trace_gpu.*.
#pragma once

#include "bvh.h"
#include "plan.h"
#include "sun.h"

#include <array>
#include <optional>
#include <string>
#include <vector>

namespace photoreal
{
    // The classes a ray sees with these settings, as bits 1 << TraceClass: the world and other classes always,
    // vegetation and foliage with alpha cards, characters with characters; and kAlphaTests unless whole cards.
    uint32_t caster_classes(const TraceSettings &settings);

    // shaders/trace_cs.hlsl's cbuffer Trace at b0, which apply_trace_ps.hlsl reads at b1: 160 bytes.
    struct TraceConstants
    {
        float clip_rows[4][4];      // D3D clip (ndc x, ndc y, reversed depth, 1) to the scene's frame, homogeneous, by rows
        float eye_position[4];      // xyz: the camera in the scene's frame
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
    struct TraceKey
    {
        uint64_t generation = 0;    // the scene's
        Size render;
        std::array<float, 16> world_to_view {}, view_to_clip {};
        Vec3 toward_sun {};
        float ao_distance = 0, sun_size = 0;
        uint32_t casters = 0;
    };
    TraceKey trace_key(uint64_t generation, Size render, const Camera &camera, const Vec3 &toward_sun, const TraceSettings &settings);

    // A new generation, render size, ao distance, sun size or casters; a camera matrix element differing by more than 1e-6
    // of the matrix's largest; or the sun's direction differing by more than 1e-5 in a component.
    bool trace_reset(const TraceKey &before, const TraceKey &now);

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
    using TraceCounts = std::array<uint64_t, kTraceCounters>;

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
    };

    // One pixel's work in one dispatch, as the compute shader does it: on a world pixel with depth, one occlusion ray when
    // it has fewer than ao_samples, one sun ray when it has fewer than sun_samples, each the next of its own sequence, and
    // on the consistency grid one ray from the camera. history is (occlusion sum, sun sum, occlusion count, sun count). The
    // alpha tests run with alpha's buffer when the constants' classes ask for them; without one every triangle is whole.
    struct PixelTrace
    {
        std::array<float, 4> history {};
        bool world = false;
        std::array<uint32_t, kTraceCounters> counters {};
    };
    PixelTrace trace_pixel(const Bvh &bvh, const TraceImage &image, const TraceConstants &constants, uint32_t x, uint32_t y,
        const std::array<float, 4> &history, const std::vector<uint32_t> *alpha = nullptr);

    // shaders/trace.hlsli's ray generator, compiled as C++: the pixel's seed, the index-th point of its R2 sequence for
    // salt (0 occlusion, 1 sun), a cosine-weighted direction around a unit normal, and a direction in the cone around a unit
    // axis whose half angle has cosine cos_max.
    uint32_t pixel_seed(uint32_t x, uint32_t y, uint32_t generation);
    std::array<float, 2> sequence_point(uint32_t seed, uint32_t salt, uint32_t index);
    Vec3 cosine_direction(const Vec3 &normal, const std::array<float, 2> &u);
    Vec3 cone_direction(const Vec3 &axis, float cos_max, const std::array<float, 2> &u);
}
