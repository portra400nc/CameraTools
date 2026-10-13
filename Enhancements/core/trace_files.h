// The files EnhancementsSaveTrace writes into trace-<YYYYMMDD-HHMMSS>\, and their reading back by the Mac's reference
// tracer. Pure C++17 like scene_files.h. The add-on's writer thread and the tests' synthetic trace call the same
// functions.
#pragma once

#include "trace.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace enhancements
{
    // Everything one save holds: the frame the trace pass traced and wrote, as it found it, and how it traced it.
    struct TraceSave
    {
        Size size;                          // the render size; every image below is size's pixels, top row first
        std::vector<float> history;         // 4 a pixel: occlusion sum, sun sum, occlusion count, sun count
        std::vector<float> depth;           // the stored reversed depth
        std::vector<uint8_t> stencil;
        std::vector<uint32_t> normals;      // R10G10B10A2_UNORM as stored
        std::vector<uint8_t> mask;          // the game's shadow mask before any pass of ours, R8G8_UNORM, 2 bytes a pixel
        std::vector<uint8_t> irradiance;    // the game's diffuse irradiance before any pass of ours, R11G11B10_FLOAT, 4 bytes a pixel
        TraceConstants constants {};        // the last ones the trace's dispatch uploaded, tiles and all: those its history was traced with
        TraceState state = TraceState::ready;   // stale when the scene had gone stale: the history is what the trace had then
        std::string why;                    // why it went stale; empty while ready
        TraceSettings settings;
        float level = 1;                    // ambient.level, which the traced occlusion is applied with
        Camera camera {};                   // the pushed camera of the frame
        Vec3d reference {};                 // the scene's frame: the first saved face's camera
        std::optional<SunCbuffers> sun;     // the newest shadow-mask constants read back, when there were any
        std::string sun_source;             // "game" (cascade 0's depth gradient), "pushed" (the camera's), or "none"
        std::optional<Vec3> floating_origin;    // b1 row 10 when the scene was built
        std::string scene;                  // the scene folder's name, which holds trace-scene.bin
        uint64_t generation = 0;
        uint64_t bvh_hash = 0;
        uint32_t triangles = 0, nodes = 0;
        uint32_t passes = 0;                // full passes: samples every traced pixel had at the save
        TraceCounts counters {};            // every dispatch's, since the samples last started again
        double mrays = 0;                   // the last measured rays a second, in millions
        uint64_t frame = 0;                 // the add-on's frame count of the saved frame

        // The gather's, when the trace gathered: its settings, the constants its last dispatch uploaded, its two buffers of
        // 4 floats a pixel (the hits' light summed and the sun-lit hits; the misses' sky summed and the misses), the
        // samples every pixel gets and the error at the last full pass, and what of the scene's light came from the game.
        bool gathering = false;
        GatherSettings gather;
        GatherConstants gather_constants {};
        std::vector<float> gather_hit, gather_sky;
        uint32_t target = 0;
        double error = 0;
        uint32_t light = 0;                 // kLightAlbedo and the rest
        // The view's G-buffer albedo at the combine step (R8G8B8A8, sRGB), its HDR scene at the bloom step before any pass
        // of ours (R11G11B10_FLOAT), and the diffuse irradiance the trace pass wrote (R11G11B10_FLOAT), when copied.
        std::vector<uint8_t> albedo, hdr, irradiance_out;
    };

    // history.f32, depth.r32, stencil.r8, normals.r10g10b10a2, mask.r8g8 and irradiance.r11g11b10 (each when the save
    // holds it), constants.bin (160 bytes), sun.bin (2224 bytes, when there were constants) and trace.tsv (one key and value
    // a line); when it gathered, gather-hit.f32, gather-sky.f32 and gather.bin (112 bytes); and albedo.rgba8,
    // hdr.r11g11b10 and irradiance-out.r11g11b10 when it holds them. Creates the folder.
    // Returns the names of the files that could not be written.
    std::vector<std::string> write_trace_folder(const std::filesystem::path &folder, const TraceSave &save);

    // trace.tsv's text.
    std::string trace_tsv(const TraceSave &save);

    // The folder back, as the reference tracer needs it: the size, the images, the constants, the scene's name, the
    // reference, the hash, the passes, the state and why, the pushed camera's matrices from trace.tsv, and the gather's
    // settings, constants and buffers when it gathered. Empty, with why, when a file is missing or of the wrong size.
    std::optional<TraceSave> read_trace_folder(const std::filesystem::path &folder, std::string &why);
}
