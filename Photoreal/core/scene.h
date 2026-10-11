// The scene capture's decisions: the request, the faces the camera turns to, the game's camera as one clip-to-world
// transform, the record of each replayed G-buffer draw, the check of its geometry against the depth buffer, the verdict
// on a captured frame, and the schedule of presents. Pure C++17 like plan.h. The shell replays, copies and reads back;
// this says when and what, and core/scene_files.* writes the result.
#pragma once

#include "plan.h"
#include "sun.h"

#include <array>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

struct PhotorealSceneRequest;  // photoreal.h, read only by parse_scene_request

namespace photoreal
{
    using Vec3d = std::array<double, 3>;
    // Row-major, applied to column vectors: m[row * 4 + column].
    using Mat4d = std::array<double, 16>;

    // PHOTOREAL_FACE_*: six world-axis cube faces, then the view.
    constexpr uint32_t kFaceCount = 7;
    constexpr uint32_t kFaceView = 6;
    constexpr uint32_t kFaceNone = 0xFFFFFFFFu;

    struct FaceSpec
    {
        const char *name;   // in file names: "pos-x"
        const char *label;  // in messages: "+X"
        Vec3d forward, up;  // Unity world space; zero for the view, which keeps the camera as it is
    };

    // D3D cube-map order and up vectors, so a later step can resample the same frames into a cube.
    extern const std::array<FaceSpec, kFaceCount> kFaces;

    // The domain form of PhotorealSceneRequest. Built only by parse_scene_request, so every value in it is in range.
    struct SceneRequest
    {
        uint32_t faces = 0x7F;          // bits 1 << face
        uint32_t settle = 45;           // presents a turned face is held before its capture
        bool raw = false;               // PHOTOREAL_SCENE_RAW
        bool ply = false;               // PHOTOREAL_SCENE_PLY
        bool trace = false;             // PHOTOREAL_SCENE_TRACE
        bool no_meshes = false;         // PHOTOREAL_SCENE_NO_MESHES
        bool gbuffer = false;           // PHOTOREAL_SCENE_GBUFFER
        bool release = false;           // PHOTOREAL_SCENE_RELEASE
        uint32_t max_triangles = 4000000;

        // A trace request with no faces: the self-test scene, captured from nothing.
        bool self_test() const { return trace && faces == 0; }
    };

    // Honors size like parse_settings: fields past it keep their defaults. faces 0 reads as all seven, except with
    // PHOTOREAL_SCENE_TRACE, where it asks for the self-test scene, and unknown bits are dropped; settle 0 reads as 45 and
    // clamps to 2..600; max_triangles 0 reads as 4000000; unknown flags are dropped. A release ignores the rest. Empty
    // when raw is null or too short to hold faces, or names no known face.
    std::optional<SceneRequest> parse_scene_request(const PhotorealSceneRequest *raw);

    // A frame's camera as the scene capture places geometry with it.
    struct SceneCamera
    {
        enum class Source : uint8_t { game, pushed } source = Source::game;
        Vec3d position {};
        std::array<Vec3d, 3> basis {};  // right, up, forward in world space
        Mat4d clip_to_world {};         // homogeneous: D3D clip space as the game's vertex shaders write it -> world
    };

    extern const std::array<const char *, 2> kCameraSourceNames;  // "game", "pushed"

    // The game's own camera from its shadow-mask draw: inverse(view) * inverse(GL projection) * inverse(to_d3d), where
    // to_d3d turns GL clip space into the vertex shaders' (y flipped for the render texture, z reversed into 0..1).
    // Empty when a value is not finite.
    std::optional<SceneCamera> scene_camera(const SunShadowConstants &game);

    // The same camera from the shadow-mask draw's b1 row 5 and b2 rows 10 to 13 and 18 to 20 alone, checked on its own:
    // every value finite and the basis orthonormal. parse_sun_shadows also checks b3's cascades, which the camera does not
    // use, and on the Deck in build 198 it refused every scene frame at radii-order, so every face fell back to the pushed
    // camera. Empty with why set to "not-finite", "basis" or "transform" when a check fails.
    std::optional<SceneCamera> scene_camera(const SunCbuffers &bytes, std::string &why);

    // The camera CameraTools pushed: inverse(flip * view_to_clip * world_to_view), with y negated when flip is set, as the
    // game's targets are stored. Empty when that does not invert.
    std::optional<SceneCamera> scene_camera(const Camera &pushed, bool flip);

    // world = (M * clip).xyz / (M * clip).w. It never divides by the clip's own w, so a triangle corner behind the camera
    // comes back exact. Empty when |(M * clip).w| < 1e-12, a point at infinity.
    std::optional<Vec3d> clip_to_world(const SceneCamera &camera, const std::array<float, 4> &clip);

    // Empty when m is singular.
    std::optional<Mat4d> invert(const Mat4d &m);

    // Whether the camera shows face: its forward within about 1.8 degrees of the face's (dot 0.9995) and its up within
    // about 2.6 (dot 0.999). Always true for the view.
    bool camera_faces(const SceneCamera &camera, uint32_t face);

    // How a G-buffer draw reached the game, as ReShade reported it.
    enum class ReplayKind : uint8_t { draw, indexed, indirect, indirect_indexed, count };
    extern const std::array<const char *, static_cast<size_t>(ReplayKind::count)> kReplayKindNames;

    // Why a G-buffer draw was not replayed. The names follow in kDrawSkipNames.
    enum class DrawSkip : uint8_t
    {
        none,
        no_bytecode,        // its vertex or domain shader was created before the add-on loaded
        gs_bound,           // the game bound a geometry shader of its own
        no_position,        // its last stage before the rasterizer writes no SV_Position
        game_so,            // the game had a stream-output target bound, which a replay would clobber
        query_pool,         // more replays this frame than queries
        gs_create_failed,   // CreateGeometryShaderWithStreamOutput refused this shader
        topology,           // not a triangle list nor a tessellated patch list: strips, adjacency, points or lines
        buffer_full,        // its reservation did not fit in the stream-output buffer, which grows for the next capture
        count,
    };
    extern const std::array<const char *, static_cast<size_t>(DrawSkip::count)> kDrawSkipNames;

    // The material each draw's triangles get, by its stencil class, as the stencil debug view colors them: the depth
    // prepass (stencil off, depth write on), the census's stencil references, and any other.
    enum class DrawClass : uint8_t { pre, s128, s129, s133, s136, s137, s144, s165, other, count };
    extern const std::array<const char *, static_cast<size_t>(DrawClass::count)> kDrawClassNames;

    // One G-buffer draw of a captured frame.
    struct SceneDrawRecord
    {
        uint32_t index = 0;                 // its place among the frame's G-buffer draws, from 0
        ReplayKind kind = ReplayKind::indexed;
        uint32_t count = 0;                 // indices or vertices per instance; direct calls only
        uint32_t instances = 0;
        uint32_t first = 0;                 // the first index or vertex
        int32_t base_vertex = 0;
        uint32_t first_instance = 0;
        uint32_t args_offset = 0;           // indirect calls only: the arguments' byte offset
        uint32_t topology = 0;              // D3D11_PRIMITIVE_TOPOLOGY
        uint64_t vs = 0, ds = 0, ps = 0;    // FNV-1a of each bound shader's bytecode, 0 when none or unknown
        bool stencil = false;
        uint32_t stencil_ref = 0;
        uint32_t depth_func = 0;            // D3D11_COMPARISON_FUNC, 0 while the depth test is off
        bool depth_write = false;
        std::array<Texture, 8> ps_inputs {};    // t0 to t7, as NativeDraw reads them
        DrawSkip skip = DrawSkip::none;
        uint32_t query = 0;                 // its stream-output statistics query, for the shell
        // Replayed as a point list, one corner per stream-output primitive, so every corner of every triangle is written;
        // false for a tessellated draw, which streams the domain shader's triangles.
        bool points = false;
        uint64_t offset = 0, reserved = 0;  // where in the stream-output buffer its replay wrote and the room it had, in corners
        uint64_t corners = 0, corners_needed = 0;   // corners its query says it wrote and would have needed
        uint64_t unwritten = 0;             // of its written corners, those still holding the clear marker after the frame
        uint64_t written = 0, needed = 0;   // triangles stream output wrote and would have needed
        uint64_t first_triangle = 0;        // where its triangles start in the captured stream
    };

    // "pre" for the depth prepass, "equal" for a draw on the prepass's depth, else "main".
    const char *draw_kind(const SceneDrawRecord &record);
    DrawClass draw_class(const SceneDrawRecord &record);
    // count / 3 * instances for a direct triangle list, (count - 2) * instances for a strip; empty for indirect,
    // tessellated and other draws, whose triangles only the GPU knows.
    std::optional<uint64_t> expected_triangles(const SceneDrawRecord &record);

    // A stream-output corner: one SV_Position, a float4.
    constexpr uint64_t kCornerBytes = 16;
    // The value the stream-output buffer is cleared to before a captured frame, in each of a corner's four lanes: a NaN
    // no vertex shader writes. Zero would not do, since shaders cull a vertex by writing a zero position.
    constexpr uint32_t kUnwrittenBits = 0x7FA5C0DEu;
    // Corners reserved for a draw whose triangle count only the GPU knows, indirect or tessellated: 16384 triangles. The
    // Deck's 350 indirect draws over seven faces in build 198 wrote at most 11,842.
    constexpr uint64_t kUnknownCorners = 3 * 16384;

    // Reservations learned from draws that overran theirs, in corners, by reserve_key. They last for a whole scene capture,
    // so an indirect draw that overran on one face starts large on the next.
    using ReserveHints = std::unordered_map<uint64_t, uint64_t>;

    // What identifies a draw across captures: its call, arguments, topology and shaders.
    uint64_t reserve_key(const SceneDrawRecord &record);

    // The corners a replay may write: count * instances for a direct draw replayed as points, else the larger of
    // kUnknownCorners and its hint.
    uint64_t corners_to_reserve(const SceneDrawRecord &record, const ReserveHints &hints);

    // The stream-output buffer of one captured frame, filled front to back in draw order, in corners.
    struct StreamLayout
    {
        uint64_t capacity = 0;  // the buffer's corners
        uint64_t used = 0;      // reserved so far
        uint64_t wanted = 0;    // reserved so far had every draw fit
    };

    // Reserves the record's corners at layout.used and sets its offset and reserved. Every replay gets its own byte
    // offset, so each draw's corners are read from where it wrote them, never from where the previous draw ended. false,
    // with the skip buffer_full, when they do not fit; wanted then says how large the buffer should be.
    bool place_draw(StreamLayout &layout, SceneDrawRecord &record, const ReserveHints &hints);

    // A replayed draw whose query says it needed more corners than it had room for. Its corners past the reservation
    // landed in the next draw's, so its own cannot be trusted.
    inline bool overran(const SceneDrawRecord &record) { return record.corners_needed > record.reserved; }

    // Whether the draw's triangles are in the captured stream the files are written from: replayed, within its
    // reservation, and every corner written.
    inline bool streamed(const SceneDrawRecord &record)
    {
        return record.skip == DrawSkip::none && !overran(record) && record.unwritten == 0;
    }

    // Triangles in a replay's corners: a third of them, except for a direct point replay whose count is not a multiple of
    // 3, whose last corners of each instance make no triangle.
    uint64_t triangles_in(const SceneDrawRecord &record, uint64_t corners);

    struct StreamRead
    {
        std::vector<float> clip;        // 12 floats a triangle, the streamed draws' in draw order
        uint32_t overran = 0;           // draws that overran their reservation; their hints grew
        uint32_t unwritten = 0;         // draws with corners stream output never wrote
    };

    // After the queries set each replayed record's corners and corners_needed: counts its unwritten corners in its
    // reservation of buffer (capacity corners), sets its written, needed and first_triangle, raises the hint of a draw that
    // overran to 1.25 times what it needed, and gathers the streamed draws' triangles.
    StreamRead unpack_stream(std::vector<SceneDrawRecord> &records, const float *buffer, uint64_t capacity, ReserveHints &hints);

    struct DepthCounts
    {
        uint32_t tested = 0;
        uint32_t matched = 0;   // within max(0.05 m, 1%) of the depth buffer's range around it
        uint32_t in_front = 0;  // nearer than the buffer: wrong, or an alpha-cut corner
        uint32_t behind = 0;    // occluded, which is fine
        friend bool operator==(const DepthCounts &a, const DepthCounts &b)
        {
            return a.tested == b.tested && a.matched == b.matched && a.in_front == b.in_front && a.behind == b.behind;
        }
    };

    struct DepthCheck
    {
        std::array<DepthCounts, static_cast<size_t>(DrawClass::count)> classes {};
        uint32_t off_screen = 0;    // corners behind the camera or outside the picture
    };

    // Compares each captured corner with the main depth around its pixel, both in meters along the camera's forward axis:
    // the corner's clip w, and the buffer's reversed depth through the camera. The pixel comes from the clip the vertex
    // shader wrote, column (x / w * 0.5 + 0.5) * width and row (0.5 - 0.5 * y / w) * height, so no flip is involved. A
    // corner matches within max(0.05 m, 1%) of the depths of the 3x3 pixels around it, and is in front when nearer than
    // all of them or when none has a depth. clip holds 12 floats a triangle; depth holds width * height floats, top row
    // first. This checks the replay, not the world transform: a corner is placed by the same camera that reads the buffer.
    DepthCheck check_depth(const SceneCamera &camera, const std::vector<SceneDrawRecord> &records, const std::vector<float> &clip, Size size,
        const std::vector<float> &depth);

    // What a captured frame turned out to be. The names follow in kFaceVerdictNames.
    enum class FaceVerdict : uint8_t
    {
        ok,
        not_ready,      // a query or a copy is not finished yet: read again at the next present
        wrong_face,     // the frame's camera did not show the face
        moved,          // the camera is not where the first saved face's was
        no_gbuffer,     // the frame had no G-buffer step
        overflow,       // a draw did not fit in the buffer, which can grow, or overran its reservation, which grew
        nothing,        // no G-buffer draw could be replayed
        no_camera,      // neither the game's camera constants nor a pushed camera
        unreadable,     // the copies stayed unreadable for kSceneReads presents
        count,
    };
    extern const std::array<const char *, static_cast<size_t>(FaceVerdict::count)> kFaceVerdictNames;

    struct Judgement
    {
        FaceVerdict verdict = FaceVerdict::ok;
        std::string why;    // the details for a message, such as the distance the camera moved
    };

    // The verdict on a frame read back for face, in this order: no G-buffer step, no draw replayed, no camera, the camera
    // facing elsewhere, the camera more than 1 mm from reference (the first saved face's), an overflow that a larger
    // buffer or reservation can fix (can_grow), and ok. A frame that overflowed at the largest buffer is ok, and saved
    // partial.
    Judgement judge_face(uint32_t face, const std::vector<SceneDrawRecord> &records, bool gbuffer, bool overflowed, bool can_grow,
        const std::optional<SceneCamera> &camera, const std::optional<Vec3d> &reference);

    // Presents the view face waits before its capture, since the camera does not turn.
    constexpr uint32_t kViewSettle = 2;
    // Presents between a capture's copies and reading them, so mapping does not wait, as for the comparison capture.
    constexpr uint32_t kSceneReadDelay = 2;
    // Presents a capture's copies may stay unread before the capture counts as failed.
    constexpr uint32_t kSceneReads = 60;
    // Captures of one face before the scene capture stops.
    constexpr uint32_t kSceneAttempts = 8;
    // Presents one face may take from its publish before the scene capture stops.
    constexpr uint32_t kSceneFacePresents = 900;
    // Faces handed to the writer and not yet written, at which the next capture waits, so at most two faces' bytes wait.
    constexpr uint32_t kSceneBacklog = 2;

    // A scene capture's progress, on the render thread.
    struct SceneSchedule
    {
        enum class Phase : uint8_t { settle, copy, read, done };

        SceneRequest request;
        std::vector<uint32_t> order;    // the faces in capture order: the view first, then 0 to 5
        size_t next = 0;                // the face being captured, in order
        Phase phase = Phase::settle;
        uint32_t since = 0;             // presents in this phase
        uint32_t presents = 0;          // presents since this face was published
        uint32_t attempts = 0;          // captures of this face so far
        uint32_t reads = 0;             // reads of this capture that were not ready
        uint32_t saved = 0;             // faces read back whole and handed to the writer

        uint32_t face() const { return next < order.size() ? order[next] : kFaceNone; }
        // The shell reads the capture back at this present, before it ticks.
        bool wants_read() const { return phase == Phase::read && since >= kSceneReadDelay; }
    };

    SceneSchedule start_scene(const SceneRequest &request);

    // What the shell learned at this present.
    struct SceneFacts
    {
        std::optional<Judgement> read;      // the verdict on the capture read back now, when wants_read() was true
        uint32_t backlog = 0;               // faces handed to the writer and not yet written
        std::optional<std::string> stop;    // a reason the shell must stop: a trip, the device gone, no stream output
    };

    // What the shell does at this present, after the read.
    struct SceneTick
    {
        uint32_t publish_face = kFaceNone;  // PHOTOREAL_FACE_* the camera should show, into the status
        bool capture_next_frame = false;    // replay the G-buffer draws of the frame that starts after this present
        bool copy_at_present = false;       // copy the stream output, the depth and the back buffer of the frame this ends
        bool done = false;                  // every face is saved or handed to the writer
        std::optional<std::string> stop;    // why the capture stops; what was saved stays
    };

    // One present. A face is published at its first present, waits settle presents (kViewSettle for the view), and is
    // captured at the next present while the writer's backlog is below kSceneBacklog. Its frame is copied at the present
    // that ends it and read kSceneReadDelay presents later. ok publishes the next face at once; wrong_face, no_gbuffer,
    // overflow and unreadable capture again at the next present, up to kSceneAttempts captures; moved, nothing and
    // no_camera stop at once, as does any face kSceneFacePresents presents after its publish.
    SceneTick scene_tick(SceneSchedule &schedule, const SceneFacts &facts);
}
