// The scene capture's files: the OBJ and PLY bytes of two triangles that share an edge, the TSV rows, the half-size
// picture, and a synthetic scene folder written by the add-on's own writer, which run.sh hands to check_scene.py.
#include "../core/scene_files.h"
#include "../photoreal.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

using namespace photoreal;

namespace
{
    int failures = 0;

    void check(const char *name, bool ok)
    {
        std::printf("%s %s\n", ok ? "PASS" : "FAIL", name);
        failures += ok ? 0 : 1;
    }

    std::string slurp(const std::filesystem::path &path)
    {
        std::ifstream file(path, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    }

    void set_unity(float out[16], const double rows[16])
    {
        for (int r = 0; r < 4; r++)
            for (int c = 0; c < 4; c++)
                out[c * 4 + r] = float(rows[r * 4 + c]);
    }

    // A camera at the origin looking down -z with a 90-degree reversed infinite projection, not flipped: clip x and y are
    // view x and y, z is 0.25 and w the distance.
    Camera simple_pushed()
    {
        Camera c {};
        const double view[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
        const double projection[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0.25, 0, 0, -1, 0 };
        set_unity(c.world_to_view, view);
        set_unity(c.view_to_clip, projection);
        return c;
    }

    void put(std::vector<float> &clip, std::initializer_list<float> v) { clip.insert(clip.end(), v.begin(), v.end()); }

    // The synthetic scene: a room 20 x 7.5 x 16 m around a camera and a 2 m cube in it, seen from all seven faces at the
    // game's conventions, with each face's depth buffer ray cast exactly. It exercises the writer, both camera sources
    // and corners behind the camera, and gives check_scene.py a folder whose every check must pass.
    namespace synthetic
    {
        constexpr uint32_t kWidth = 320, kHeight = 200;
        constexpr double kNear = 0.25, kFar = 1000, kAspect = double(kWidth) / kHeight;
        const Vec3d kCamera = { 100.25, 50.5, -30.75 };
        const Vec3d kRoomMin = { kCamera[0] - 8, kCamera[1] - 1.5, kCamera[2] - 9 }, kRoomMax = { kCamera[0] + 12, kCamera[1] + 6, kCamera[2] + 7 };
        const Vec3d kCubeMin = { kCamera[0] + 2, kCamera[1] - 1, kCamera[2] - 5 }, kCubeMax = { kCamera[0] + 4, kCamera[1] + 1, kCamera[2] - 3 };

        Vec3d sub(const Vec3d &a, const Vec3d &b) { return { a[0] - b[0], a[1] - b[1], a[2] - b[2] }; }
        Vec3d cross(const Vec3d &a, const Vec3d &b) { return { a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0] }; }
        double dot(const Vec3d &a, const Vec3d &b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

        struct Triangle { Vec3d a, b, c; };

        // One side of a box, split into n x n quads, turned so cross(b - a, c - a) points toward facing: Unity's front
        // faces wind clockwise in its left-handed space, which is this cross pointing at the viewer.
        void side(std::vector<Triangle> &out, const Vec3d &lo, const Vec3d &hi, int axis, bool at_max, const Vec3d &facing, int n)
        {
            const int u = (axis + 1) % 3, v = (axis + 2) % 3;
            for (int i = 0; i < n; i++)
                for (int j = 0; j < n; j++)
                {
                    const auto corner = [&](int di, int dj) {
                        Vec3d p;
                        p[axis] = at_max ? hi[axis] : lo[axis];
                        p[u] = lo[u] + (hi[u] - lo[u]) * (i + di) / n;
                        p[v] = lo[v] + (hi[v] - lo[v]) * (j + dj) / n;
                        return p;
                    };
                    for (const Triangle &t : { Triangle { corner(0, 0), corner(1, 0), corner(1, 1) }, Triangle { corner(0, 0), corner(1, 1), corner(0, 1) } })
                        out.push_back(dot(cross(sub(t.b, t.a), sub(t.c, t.a)), facing) >= 0 ? t : Triangle { t.a, t.c, t.b });
                }
        }

        // The ray's nearest hit inside the room or on the cube.
        double cast(const Vec3d &from, const Vec3d &direction)
        {
            double nearest = INFINITY;
            for (int k = 0; k < 3; k++)
                if (direction[k] != 0)
                    nearest = std::min(nearest, ((direction[k] > 0 ? kRoomMax[k] : kRoomMin[k]) - from[k]) / direction[k]);
            double enter = -INFINITY, leave = INFINITY;
            for (int k = 0; k < 3; k++)
            {
                const double t0 = (kCubeMin[k] - from[k]) / direction[k], t1 = (kCubeMax[k] - from[k]) / direction[k];
                enter = std::max(enter, std::min(t0, t1));
                leave = std::min(leave, std::max(t0, t1));
            }
            return enter <= leave && enter > 0 ? std::min(nearest, enter) : nearest;
        }

        void write(const std::filesystem::path &folder_path)
        {
            std::vector<std::vector<Triangle>> draws(7);
            const int n = 8;
            for (int axis = 0; axis < 3; axis++)
                for (bool at_max : { false, true })
                {
                    Vec3d inward {};
                    inward[axis] = at_max ? -1 : 1;
                    side(draws[axis * 2 + at_max], kRoomMin, kRoomMax, axis, at_max, inward, n);
                    Vec3d outward {};
                    outward[axis] = -inward[axis];
                    side(draws[6], kCubeMin, kCubeMax, axis, at_max, outward, 2);
                }

            SceneFolder folder;
            folder.path = folder_path;
            begin_scene_folder(folder);
            SceneRequest request;
            const uint32_t order[] = { kFaceView, 0, 1, 2, 3, 4, 5 };
            for (uint32_t face : order)
            {
                Vec3d forward = kFaces[face].forward, up = kFaces[face].up;
                if (face == kFaceView)
                {
                    // 30 degrees left of -Z and 10 degrees down.
                    const double yaw = -150 * M_PI / 180, pitch = -10 * M_PI / 180;
                    forward = { std::cos(pitch) * std::sin(yaw), std::sin(pitch), std::cos(pitch) * std::cos(yaw) };
                    up = { -std::sin(pitch) * std::sin(yaw), std::cos(pitch), -std::sin(pitch) * std::cos(yaw) };
                }
                const Vec3d right = cross(up, forward);
                // Unity's worldToCameraMatrix and GL.GetGPUProjectionMatrix's reversed-Z projection, 90 degrees vertical.
                double view[16] = { right[0], right[1], right[2], -dot(right, kCamera), up[0], up[1], up[2], -dot(up, kCamera), -forward[0], -forward[1],
                    -forward[2], dot(forward, kCamera), 0, 0, 0, 1 };
                const double projection[16] = { 1 / kAspect, 0, 0, 0, 0, 1, 0, 0, 0, 0, kNear / (kFar - kNear), kFar * kNear / (kFar - kNear), 0, 0, -1, 0 };
                Camera pushed {};
                set_unity(pushed.world_to_view, view);
                set_unity(pushed.view_to_clip, projection);
                // Even faces are placed by a game camera built as parse_sun_shadows would leave it, odd ones by the pushed
                // camera, so both sources write the same scene.
                std::optional<SceneCamera> camera;
                std::optional<SunCbuffers> sun;
                if (face % 2 == 0)
                {
                    SunShadowConstants game;
                    game.camera = { float(kCamera[0]), float(kCamera[1]), float(kCamera[2]) };
                    game.basis = { { { float(right[0]), float(right[1]), float(right[2]) }, { float(up[0]), float(up[1]), float(up[2]) },
                        { float(forward[0]), float(forward[1]), float(forward[2]) } } };
                    // The GL projection is [[1,0,0,0],[0,1,0,0],[0,0,-2,1],[0,0,0,1]] times the GPU one; b2 holds its
                    // inverse's columns.
                    Mat4d gl {};
                    for (int c = 0; c < 4; c++)
                    {
                        gl[c] = projection[c];
                        gl[4 + c] = projection[4 + c];
                        gl[8 + c] = -2 * projection[8 + c] + projection[12 + c];
                        gl[12 + c] = projection[12 + c];
                    }
                    const Mat4d inverse = *invert(gl);
                    for (int column = 0; column < 4; column++)
                        for (int row = 0; row < 4; row++)
                            game.inverse_projection[column][row] = float(inverse[row * 4 + column]);
                    // The shadow-mask draw's b1 row 5 and b2 rows 10 to 13 and 18 to 20, which the add-on reads.
                    SunCbuffers bytes {};
                    std::memcpy(bytes.data() + 768 + 5 * 16, game.camera.data(), sizeof game.camera);
                    for (int i = 0; i < 4; i++)
                        std::memcpy(bytes.data() + 960 + (10 + i) * 16, game.inverse_projection[i].data(), sizeof game.inverse_projection[i]);
                    for (int i = 0; i < 3; i++)
                        std::memcpy(bytes.data() + 960 + (18 + i) * 16, game.basis[i].data(), sizeof game.basis[i]);
                    std::string why;
                    camera = scene_camera(bytes, why);
                    sun = bytes;
                }
                else
                    camera = scene_camera(pushed, true);

                // The game's vertex shaders write flip * projection * view.
                double to_clip[16] = {};
                for (int r = 0; r < 4; r++)
                    for (int c = 0; c < 4; c++)
                        for (int k = 0; k < 4; k++)
                            to_clip[r * 4 + c] += projection[r * 4 + k] * view[k * 4 + c];
                for (int c = 0; c < 4; c++)
                    to_clip[4 + c] = -to_clip[4 + c];

                SceneFaceData data;
                data.face = face;
                data.frame = 1000 + face;
                data.attempts = 1;
                data.camera = *camera;
                data.camera_note = face % 2 == 0 ? "" : "synthetic";
                data.pushed = pushed;
                data.sun = sun;
                data.status = "320x200 synthetic";
                data.render = { kWidth, kHeight };
                data.origin = kCamera;
                uint64_t triangles = 0;
                for (size_t d = 0; d < draws.size(); d++)
                {
                    SceneDrawRecord record;
                    record.index = uint32_t(d);
                    record.stencil = true;
                    record.stencil_ref = d < 6 ? 128 : 133;
                    record.depth_func = 7;
                    record.topology = 4;
                    record.first_triangle = triangles;
                    for (const Triangle &t : draws[d])
                    {
                        // Unity culls whole objects; here each triangle not wholly outside one frustum plane is drawn,
                        // corners behind the camera included.
                        std::array<std::array<float, 4>, 3> corners;
                        uint32_t outside[5] = {};
                        for (int k = 0; k < 3; k++)
                        {
                            const Vec3d &p = k == 0 ? t.a : k == 1 ? t.b : t.c;
                            for (int r = 0; r < 4; r++)
                                corners[k][r] = float(to_clip[r * 4] * p[0] + to_clip[r * 4 + 1] * p[1] + to_clip[r * 4 + 2] * p[2] + to_clip[r * 4 + 3]);
                            const std::array<float, 4> &q = corners[k];
                            outside[0] += q[0] > q[3];
                            outside[1] += q[0] < -q[3];
                            outside[2] += q[1] > q[3];
                            outside[3] += q[1] < -q[3];
                            outside[4] += q[3] < kNear;
                        }
                        if (std::find(std::begin(outside), std::end(outside), 3u) != std::end(outside))
                            continue;
                        for (const auto &q : corners)
                            data.clip.insert(data.clip.end(), q.begin(), q.end());
                        record.written++;
                    }
                    record.needed = record.written;
                    record.count = uint32_t(record.written * 3);
                    record.instances = 1;
                    triangles += record.written;
                    data.records.push_back(record);
                }

                data.depth.resize(size_t(kWidth) * kHeight);
                data.rgb.resize(size_t(kWidth) * kHeight * 3);
                data.picture = { kWidth, kHeight };
                for (uint32_t py = 0; py < kHeight; py++)
                    for (uint32_t px = 0; px < kWidth; px++)
                    {
                        const float ndc_x = (px + 0.5f) / kWidth * 2 - 1, ndc_y = 1 - (py + 0.5f) / kHeight * 2;
                        const Vec3d at = *clip_to_world(*camera, { ndc_x, ndc_y, 1, 1 });
                        const Vec3d direction = sub(at, kCamera);
                        const double t = cast(kCamera, direction);
                        const double distance = t * dot(direction, forward);
                        data.depth[size_t(py) * kWidth + px] = float(kNear * (kFar - distance) / ((kFar - kNear) * distance));
                        const uint8_t shade = uint8_t(255 * std::exp(-distance / 15));
                        std::fill_n(&data.rgb[(size_t(py) * kWidth + px) * 3], 3, shade);
                    }
                write_scene_face(folder, data, request);
            }
        }
    }
}

int main(int argc, char **argv)
{
    const SceneCamera camera = *scene_camera(simple_pushed(), false);
    // (1, 2, -10), (2, 2, -10), (1, 3, -10) and (2, 3, -10) in two triangles that share the edge from the second to the third.
    std::vector<float> clip;
    put(clip, { 1, 2, 0.25f, 10, 2, 2, 0.25f, 10, 1, 3, 0.25f, 10 });
    put(clip, { 2, 2, 0.25f, 10, 2, 3, 0.25f, 10, 1, 3, 0.25f, 10 });
    SceneDrawRecord r;
    r.index = 3;
    r.kind = ReplayKind::indexed;
    r.count = 6;
    r.instances = 1;
    r.first = 12;
    r.base_vertex = -4;
    r.topology = 4;
    r.vs = 0xab;
    r.ps = 0xcd;
    r.stencil = true;
    r.stencil_ref = 128;
    r.depth_func = 4;
    r.ps_inputs[0] = { ResourceId { 0x1f00 }, Format::r8g8b8a8_unorm, { 1920, 1200 } };
    r.written = r.needed = 2;
    SceneDrawRecord skipped;
    skipped.index = 4;
    skipped.kind = ReplayKind::indirect_indexed;
    skipped.args_offset = 20;
    skipped.topology = 4;
    skipped.stencil = true;
    skipped.stencil_ref = 137;
    skipped.depth_func = 3;
    skipped.skip = DrawSkip::no_bytecode;
    const std::vector<SceneDrawRecord> records = { r, skipped };
    const Vec3d origin = { 0.5, 0, 0 };
    {
        std::ostringstream out;
        const MeshStats stats = write_obj(out, camera, origin, records, clip, 4000000);
        check("two triangles sharing an edge: 4 vertices relative to the origin with z negated, and 2 faces with their winding swapped",
            out.str() == "mtllib scene.mtl\ng d3_main_s128_vs00000000000000ab\nusemtl s128\nv 0.500 2.000 10.000\nv 1.500 2.000 10.000\nv 0.500 3.000 10.000\n"
                         "f 1 3 2\nv 1.500 3.000 10.000\nf 2 3 4\n");
        check("the OBJ's stats: 2 triangles, 4 vertices, none dropped or past the limit", stats.triangles == 2 && stats.vertices == 4 && stats.dropped == 0
            && stats.past_max == 0);
        std::ostringstream ply;
        write_ply(ply, camera, origin, records, clip, 4000000);
        std::string want = "ply\nformat binary_little_endian 1.0\nelement vertex 4\nproperty float x\nproperty float y\nproperty float z\nelement face 2\n"
                           "property list uchar int vertex_indices\nend_header\n";
        for (float f : { 0.5f, 2.0f, 10.0f, 1.5f, 2.0f, 10.0f, 0.5f, 3.0f, 10.0f, 1.5f, 3.0f, 10.0f })
            want.append(reinterpret_cast<const char *>(&f), 4);
        for (const std::array<int32_t, 3> &face : { std::array<int32_t, 3> { 0, 2, 1 }, std::array<int32_t, 3> { 1, 2, 3 } })
        {
            want += '\3';
            want.append(reinterpret_cast<const char *>(face.data()), 12);
        }
        check("the same as binary PLY: header, 4 float vertices, 2 triangles of a uchar 3 and three ints from 0", ply.str() == want);
        std::ostringstream capped;
        const MeshStats past = write_obj(capped, camera, origin, records, clip, 1);
        check("a 1-triangle limit leaves the whole 2-triangle draw out and counts it", past.triangles == 0 && past.past_max == 2
            && capped.str() == "mtllib scene.mtl\n");
        std::vector<float> infinite = clip;
        infinite[3] = 0;
        infinite[2] = 0;
        std::ostringstream dropped;
        const MeshStats at_infinity = write_obj(dropped, camera, origin, records, infinite, 4000000);
        check("a corner at infinity drops its triangle and keeps the other", at_infinity.triangles == 1 && at_infinity.dropped == 1);
    }
    {
        check("draws.tsv: a direct draw with its arguments, expected and written triangles and its first input, and a skipped indirect draw",
            draw_rows(0, records) == "0\t3\tmain\ts128\tindexed\t6\t1\t12\t-4\t0\t-\t4\t00000000000000ab\t-\t00000000000000cd\t1\t128\tless-equal\t0\t2\t2\t2\t0\t-"
                                     "\t1f00:28:1920x1200\t-\t-\t-\t-\t-\t-\t-\n"
                                     "0\t4\tequal\ts137\tindirect-indexed\t-\t-\t-\t-\t-\t20\t4\t-\t-\t-\t1\t137\tequal\t0\t-\t0\t0\t0\tno-bytecode\t-\t-\t-\t-\t-\t-\t-\t-\n");
        SceneFaceData data;
        data.face = kFaceView;
        data.frame = 1234;
        data.attempts = 2;
        data.camera = camera;
        data.camera_note = "sun constants unread";
        data.status = "320x200 found gbuffer";
        data.render = { 4, 4 };
        data.records = records;
        data.clip = clip;
        data.depth.assign(16, 0.025f);
        data.origin = origin;
        data.pushed = simple_pushed();
        std::ostringstream out;
        const MeshStats stats = write_obj(out, camera, origin, records, clip, 4000000);
        const std::string row = scene_row(data, stats, check_depth(camera, records, clip, data.render, data.depth));
        check("scene.tsv: the face, its frame and camera, 1 replayed and 1 skipped for no-bytecode, triangles, 6 corners matched at 10 m, and the camera",
            row == "6\tview\t1234\t2\tpushed\tsun constants unread\t320x200 found gbuffer\t4\t4\t2\t1\t1\t0\t0\t0\t0\t0\t0\t2\t2\t0\t2\t4\t0\t0"
                   "\t0\t0\t0\t0\t6\t6\t0\t0\t0\t0\t0\t0\t0\t0\t0\t0\t0\t0\t0\t0\t0\t0\t0\t0\t0\t0\t0\t0\t0\t0\t0\t0\t0\t0\t0\t0"
                   "\t0\t0.5,0,0\t0,0,0\t0,0,-1\t0,1,0\n");
        check("camera.tsv: no shadow-mask constants, then clip_to_world and the pushed matrices row by row",
            camera_row(data) == "6\tpushed\t-\t-\t-\t-\t-\t-\t-\t-\t-\t-\t-\t-\t-\t1,0,0,0,0,1,0,0,0,0,0,-1,0,0,4,0\t1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1"
                                "\t1,0,0,0,0,1,0,0,0,0,0,0.25,0,0,-1,0\n");
        check("the headers have one column per cell", std::count(kSceneHeader, kSceneHeader + std::strlen(kSceneHeader), '\t')
            == std::count(row.begin(), row.end(), '\t') && std::string(kCameraHeader).find("\tb2_20\tclip_to_world") != std::string::npos);
        check("mesh names", mesh_name(0, false) == "face-0-pos-x.obj" && mesh_name(6, true) == "face-6-view.ply");
        check("scene.mtl names every class with the stencil view's colors", scene_mtl().find("newmtl s128\nKd 0.5 0.5 0.5\n") != std::string::npos
            && scene_mtl().find("newmtl pre\nKd 1 0 0\n") != std::string::npos && scene_mtl().find("newmtl other\nKd 0.55 0.35 0.15\n") != std::string::npos);
    }
    {
        // 3x2 pixels: the odd column is dropped, each output pixel averages a 2x2 block, rounding to nearest.
        const std::vector<uint8_t> rgb = { 0, 10, 255, 1, 20, 255, 9, 9, 9, 2, 30, 0, 4, 40, 0, 9, 9, 9 };
        Size half;
        const std::vector<uint8_t> small = half_size(rgb, { 3, 2 }, &half);
        check("half size of 3x2 is 1x1, the mean of the left 2x2 block", half == Size { 1, 1 } && small == std::vector<uint8_t> { 2, 25, 128 });
    }
    if (argc > 1)
    {
        const std::filesystem::path folder = std::filesystem::path(argv[1]) / "scene-synthetic";
        synthetic::write(folder);
        const std::string scene = slurp(folder / "scene.tsv");
        check("the synthetic scene folder has seven faces in scene.tsv, each with its OBJ, picture and depth",
            std::count(scene.begin(), scene.end(), '\n') == 8 && std::filesystem::file_size(folder / "face-6-view.obj") > 0
                && std::filesystem::file_size(folder / "face-3-depth.r32") == synthetic::kWidth * synthetic::kHeight * 4
                && std::filesystem::exists(folder / "face-2.png") && std::filesystem::exists(folder / "scene.mtl"));
        check("faces placed by the game's camera keep the shadow-mask draw's b0 to b3 as face-N-sun.bin, the others have none",
            std::filesystem::file_size(folder / "face-0-sun.bin") == kSunCbufferTotal && !std::filesystem::exists(folder / "face-1-sun.bin"));
    }
    std::printf("%s: %d failed\n", __FILE__, failures);
    return failures == 0 ? 0 : 1;
}
