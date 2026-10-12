// The traced scene: the triangles of a scene capture's faces in one world frame, relative to the first saved face's
// camera, deduplicated across faces on a 1 mm grid, ready for core/bvh.*'s build. Pure C++17 like scene.h. The add-on's
// writer thread adds each saved face as it writes it, and takes the scene after the last.
#pragma once

#include "scene.h"

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace photoreal
{
    using Vec3f = std::array<float, 3>;

    // What a triangle is to the trace: which settings let it cast. Characters cast but are never traced themselves, since
    // only world pixels are. The values are bits 24 to 27 of a triangle's info, which shaders/trace.hlsli reads.
    enum class TraceClass : uint8_t
    {
        world,          // stencil 128 and 144
        vegetation,     // 136, cast as full cards until alpha-tested casters come
        foliage,        // 137, the same
        other,          // any other stencil, and the depth prepass where it has no main twin
        character,      // 133 and 165
        count,
    };
    extern const std::array<const char *, static_cast<size_t>(TraceClass::count)> kTraceClassNames;

    // The class a draw's triangles get, or empty for grass (stencil 129), which never casts, as for contact shadows.
    std::optional<TraceClass> trace_class(const SceneDrawRecord &record);

    // Whether a draw leaves surfaces in the depth buffer: it writes depth, or it draws the prepass's surfaces again at
    // EQUAL depth. A draw that does neither casts nothing: in build 199's capture, a 12-triangle stencil volume around the
    // character (stencil 38, depth test ALWAYS) and 5,556 triangles of the character's overlays.
    bool leaves_depth(const SceneDrawRecord &record);

    // A triangle's info word: bits 0 to 23 its alpha record (core/alpha.h), all set for a whole triangle; bits 24 to 27 its
    // class.
    constexpr uint32_t kOpaque = kNoRecord;
    constexpr uint32_t triangle_info(TraceClass c, uint32_t record = kOpaque) { return (record & kOpaque) | static_cast<uint32_t>(c) << 24; }
    constexpr TraceClass info_class(uint32_t info) { return static_cast<TraceClass>(info >> 24 & 0xFu); }

    struct TraceTriangle
    {
        std::array<Vec3f, 3> corner {};     // meters, relative to the scene's reference, in Unity's world axes
        uint32_t info = triangle_info(TraceClass::world);
    };

    // A triangle's corners on a 1 mm grid, each coordinate rounded half up, sorted lexicographically, so the same triangle
    // seen by two faces, in any corner order, gets one key.
    struct TriangleKey
    {
        std::array<int32_t, 9> q {};
        friend bool operator==(const TriangleKey &a, const TriangleKey &b) { return a.q == b.q; }
    };

    // Empty when a coordinate is not finite or more than 2000 km from the reference, or when the grid merges two corners,
    // a triangle too small to keep.
    std::optional<TriangleKey> triangle_key(const std::array<Vec3d, 3> &corners);

    // A draw's alpha test as the traced scene keeps it: its program, its texture's atlas tile and sampler address modes, and
    // the constant rows its replay copied, 4 words a row.
    struct AlphaDraw
    {
        std::shared_ptr<const AlphaProgram> program;
        uint32_t tile = kNoTile, address = 0;
        std::vector<uint32_t> rows;
    };

    // Collects the faces' triangles in the order they arrive (the view first, then the cube faces; draws in order) and
    // keeps one of each key: of a main, an equal-depth and a prepass draw's copy the main one, else the first. An
    // equal-depth copy without an alpha test keeps the alpha test of the prepass copy it replaces: Genshin's vegetation
    // draws its prepass alpha-tested and its G-buffer at EQUAL depth with shaders that do not discard. Open
    // addressing over the keys, about 90 bytes per unique triangle, and a kept triangle with an alpha test its corners'
    // streamed floats.
    class SceneBuilder
    {
    public:
        struct Counts
        {
            uint64_t in = 0;                    // triangles offered
            uint64_t unique = 0;                // kept, one per key
            uint64_t replaced = 0;              // kept copies a later copy of a higher kind replaced
            uint64_t dropped_class = 0;         // grass
            uint64_t dropped_no_depth = 0;      // draws that leave no surface in the depth buffer
            uint64_t dropped_degenerate = 0;    // a corner at infinity, out of range, or merged with another on the grid
            uint64_t pre_only = 0;              // kept triangles only the depth prepass drew
            uint64_t past_max = 0;              // left out by take, the farthest first
            uint64_t inherited = 0;             // equal-depth copies that kept the alpha test of the prepass copy they replaced
            uint64_t alpha = 0;                 // kept with an alpha record
            uint64_t alpha_past_max = 0;        // kept whole because the records would pass 64 MB
        };

        // alpha, with attributes holding each triangle's three corners' program inputs, corner by corner, makes the draw's
        // triangles alpha-tested.
        void add_draw(const SceneDrawRecord &draw, const std::vector<std::array<Vec3d, 3>> &triangles, const AlphaDraw *alpha = nullptr,
            const float *attributes = nullptr);
        // The capture's alpha atlas so far, which every face's read holds whole.
        void set_atlas(std::vector<uint8_t> atlas) { atlas_ = std::move(atlas); }
        Counts counts() const { return counts_; }

        // The kept triangles in the order they arrived. Past max_triangles, those whose centroid lies farthest from the
        // reference are left out. With alpha, the kept triangles' alpha records go into it, as core/alpha.h lays an alpha
        // buffer out, and each triangle's info names its record; it stays empty when none has one. Empties the builder.
        std::vector<TraceTriangle> take(uint64_t max_triangles, std::vector<uint32_t> *alpha = nullptr);

    private:
        void grow();

        std::vector<TriangleKey> keys_;
        std::vector<TraceTriangle> triangles_;
        std::vector<uint8_t> ranks_;            // 0 prepass, 1 equal, 2 main
        std::vector<uint32_t> slots_;           // an index into keys_ plus 1, 0 for an empty slot
        std::vector<AlphaDraw> alpha_draws_;
        std::vector<uint32_t> alpha_of_;        // each kept triangle's alpha draw plus 1, 0 for a whole triangle
        std::vector<uint64_t> attributes_at_;   // where its corners' floats start in attributes_
        std::vector<float> attributes_;
        std::vector<uint8_t> atlas_;
        Counts counts_;
    };

    // Adds one saved face's streamed draws: each corner through camera's clip_to_world, relative to origin, in double. A
    // draw whose alpha test is a program comes with its corners' floats from attributes and its rows from constants, as
    // core/scene.h's unpack_stream and the replay's copies left them.
    void add_face(SceneBuilder &builder, const SceneCamera &camera, const Vec3d &origin, const std::vector<SceneDrawRecord> &records,
        const std::vector<float> &clip, const std::vector<float> &attributes = {}, const std::vector<uint32_t> &constants = {});

    // The self-test scene, built from constants around the reference: a roof triangle 4 m above it, a wall 5 m ahead along
    // forward (horizontal, unit length), and a plate 1 m below it 2 m ahead, all world class. It casts shadows and
    // occlusion onto whatever the frame shows, so the trace view of it proves the GPU's kernel against the Mac's.
    std::vector<TraceTriangle> self_test_scene(const Vec3d &forward);

    // trace-scene.bin: b"PRTS", a uint32 count, the reference as three float64, then per triangle nine float32 corners and
    // the uint32 info, little-endian: what the add-on uploads, before the BVH reorders it. With an alpha buffer, its uint32
    // word count and its words follow.
    std::string trace_scene_bytes(const std::vector<TraceTriangle> &triangles, const Vec3d &reference, const std::vector<uint32_t> &alpha = {});
    struct TraceSceneFile
    {
        std::vector<TraceTriangle> triangles;
        Vec3d reference {};
        std::vector<uint32_t> alpha;            // empty for a file without an alpha buffer, as T1a wrote them
    };
    // Empty when the bytes are not such a file.
    std::optional<TraceSceneFile> parse_trace_scene(const std::string &bytes);
}
