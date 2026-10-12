// The traced scene: the triangles of a scene capture's faces in one world frame, relative to the first saved face's
// camera, deduplicated across faces on a 1 mm grid, ready for core/bvh.*'s build. Pure C++17 like scene.h. The add-on's
// writer thread adds each saved face as it writes it, and takes the scene after the last.
#pragma once

#include "scene.h"

#include <array>
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
    constexpr uint32_t kOpaque = 0xFFFFFFu;
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

    // Collects the faces' triangles in the order they arrive (the view first, then the cube faces; draws in order) and
    // keeps one of each key: of a main, an equal-depth and a prepass draw's copy the main one, else the first. Open
    // addressing over the keys, about 90 bytes per unique triangle.
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
        };

        void add_draw(const SceneDrawRecord &draw, const std::vector<std::array<Vec3d, 3>> &triangles);
        Counts counts() const { return counts_; }

        // The kept triangles in the order they arrived. Past max_triangles, those whose centroid lies farthest from the
        // reference are left out. Empties the builder.
        std::vector<TraceTriangle> take(uint64_t max_triangles);

    private:
        void grow();

        std::vector<TriangleKey> keys_;
        std::vector<TraceTriangle> triangles_;
        std::vector<uint8_t> ranks_;            // 0 prepass, 1 equal, 2 main
        std::vector<uint32_t> slots_;           // an index into keys_ plus 1, 0 for an empty slot
        Counts counts_;
    };

    // Adds one saved face's streamed draws: each corner through camera's clip_to_world, relative to origin, in double.
    void add_face(SceneBuilder &builder, const SceneCamera &camera, const Vec3d &origin, const std::vector<SceneDrawRecord> &records,
        const std::vector<float> &clip);

    // The self-test scene, built from constants around the reference: a roof triangle 4 m above it, a wall 5 m ahead along
    // forward (horizontal, unit length), and a plate 1 m below it 2 m ahead, all world class. It casts shadows and
    // occlusion onto whatever the frame shows, so the trace view of it proves the GPU's kernel against the Mac's.
    std::vector<TraceTriangle> self_test_scene(const Vec3d &forward);

    // trace-scene.bin: b"PRTS", a uint32 count, the reference as three float64, then per triangle nine float32 corners and
    // the uint32 info, little-endian: what the add-on uploads, before the BVH reorders it.
    std::string trace_scene_bytes(const std::vector<TraceTriangle> &triangles, const Vec3d &reference);
    struct TraceSceneFile
    {
        std::vector<TraceTriangle> triangles;
        Vec3d reference {};
    };
    // Empty when the bytes are not such a file.
    std::optional<TraceSceneFile> parse_trace_scene(const std::string &bytes);
}
