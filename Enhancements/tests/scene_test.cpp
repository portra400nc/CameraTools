// The scene capture's decisions: the request, the game's camera and the pushed one as clip-to-world transforms checked
// against the G-buffer vertex shaders' own matrix of captures 111824 and 214859, the face check, the depth check, the
// verdict on a frame, and the schedule of presents.
#include "../core/scene.h"
#include "../enhancements.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

using namespace enhancements;

namespace
{
    int failures = 0;

    void check(const char *name, bool ok)
    {
        std::printf("%s %s\n", ok ? "PASS" : "FAIL", name);
        failures += ok ? 0 : 1;
    }

    std::vector<uint8_t> read(const std::string &path)
    {
        std::ifstream file(path, std::ios::binary);
        std::vector<uint8_t> data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        if (data.empty())
            throw std::runtime_error("missing " + path);
        return data;
    }

    // fixtures/<capture>-sun-b0.bin to b3.bin back to back, as sun_test.cpp loads them.
    SunCbuffers load_sun(const std::string &capture)
    {
        SunCbuffers bytes {};
        size_t offset = 0;
        for (int i = 0; i < 4; i++)
        {
            const std::vector<uint8_t> data = read("fixtures/" + capture + "-sun-b" + std::to_string(i) + ".bin");
            std::memcpy(bytes.data() + offset, data.data(), data.size());
            offset += data.size();
        }
        return bytes;
    }

    std::array<double, 4> row_of(const uint8_t *bytes, uint32_t row)
    {
        float f[4];
        std::memcpy(f, bytes + row * 16, sizeof f);
        return { f[0], f[1], f[2], f[3] };
    }

    // The G-buffer vertex shaders' world-to-clip transform: rows 17 to 20 of their 560-byte camera block are its columns.
    std::array<float, 4> project(const std::vector<uint8_t> &block, const Vec3d &p)
    {
        double clip[4] = {};
        for (uint32_t column = 0; column < 4; column++)
        {
            const std::array<double, 4> c = row_of(block.data(), 17 + column);
            const double weight = column < 3 ? p[column] : 1;
            for (int r = 0; r < 4; r++)
                clip[r] += c[r] * weight;
        }
        return { float(clip[0]), float(clip[1]), float(clip[2]), float(clip[3]) };
    }

    double error(const std::optional<Vec3d> &got, const Vec3d &want)
    {
        if (!got)
            return INFINITY;
        return std::sqrt(std::pow((*got)[0] - want[0], 2) + std::pow((*got)[1] - want[1], 2) + std::pow((*got)[2] - want[2], 2));
    }

    // The camera CameraTools would push for the frame, rebuilt as tracer/vp_check.py rebuilds it from b1 and b2:
    // world_to_view from the basis and position, and GL.GetGPUProjectionMatrix's reversed-Z projection from b2 rows 6 to 9.
    Camera pushed_from(const SunCbuffers &bytes)
    {
        const uint8_t *b1 = bytes.data() + 768, *b2 = b1 + 192;
        const std::array<double, 4> right = row_of(b2, 18), up = row_of(b2, 19), forward = row_of(b2, 20), position = row_of(b1, 5);
        double view[16] = {};  // row-major
        for (int k = 0; k < 3; k++)
        {
            view[0 * 4 + k] = right[k];
            view[1 * 4 + k] = up[k];
            view[2 * 4 + k] = -forward[k];
        }
        for (int r = 0; r < 3; r++)
            view[r * 4 + 3] = -(view[r * 4] * position[0] + view[r * 4 + 1] * position[1] + view[r * 4 + 2] * position[2]);
        view[15] = 1;
        double gl[16];  // row-major; rows 6 to 9 are its columns
        for (int column = 0; column < 4; column++)
        {
            const std::array<double, 4> c = row_of(b2, 6 + column);
            for (int r = 0; r < 4; r++)
                gl[r * 4 + column] = c[r];
        }
        // GPU projection = [[1,0,0,0],[0,1,0,0],[0,0,-0.5,0.5],[0,0,0,1]] * GL projection.
        double gpu[16];
        for (int c = 0; c < 4; c++)
        {
            gpu[c] = gl[c];
            gpu[4 + c] = gl[4 + c];
            gpu[8 + c] = -0.5 * gl[8 + c] + 0.5 * gl[12 + c];
            gpu[12 + c] = gl[12 + c];
        }
        Camera camera {};
        for (int r = 0; r < 4; r++)
            for (int c = 0; c < 4; c++)
            {
                camera.world_to_view[c * 4 + r] = float(view[r * 4 + c]);
                camera.view_to_clip[c * 4 + r] = float(gpu[r * 4 + c]);
            }
        return camera;
    }

    struct Capture
    {
        const char *name;
        std::vector<Vec3d> points;  // the last lies behind the camera
    };

    void check_transform(const Capture &capture)
    {
        const SunCbuffers bytes = load_sun(capture.name);
        const std::vector<uint8_t> block = read(std::string("fixtures/") + capture.name + "-vs-camera.bin");
        const SunParse parse = parse_sun_shadows(bytes);
        const std::optional<SceneCamera> game = parse.check == SunCheck::ok ? scene_camera(parse.constants) : std::nullopt;
        const std::optional<SceneCamera> pushed = scene_camera(pushed_from(bytes), true);
        double worst_game = 0, worst_pushed = 0;
        bool behind = false;
        for (const Vec3d &p : capture.points)
        {
            const std::array<float, 4> clip = project(block, p);
            behind |= clip[3] < 0;
            worst_game = std::max(worst_game, game ? error(clip_to_world(*game, clip), p) : INFINITY);
            worst_pushed = std::max(worst_pushed, pushed ? error(clip_to_world(*pushed, clip), p) : INFINITY);
        }
        const std::string name = capture.name;
        check((name + ": five points projected by the vertex shaders' matrix, one behind the camera, come back within 2 mm through the game's camera").c_str(),
            behind && worst_game < 0.002);
        check((name + ": and within 2 mm through the pushed camera rebuilt from b1 and b2").c_str(), behind && worst_pushed < 0.002);
        check((name + ": the game's camera is b1 row 5 with b2's basis, and the pushed camera sits within 1 mm of it").c_str(),
            game && pushed && game->source == SceneCamera::Source::game && pushed->source == SceneCamera::Source::pushed
                && game->position[0] == parse.constants.camera[0] && game->basis[2][2] == parse.constants.basis[2][2]
                && error(pushed->position, game->position) < 0.001 && std::fabs(pushed->basis[2][1] - game->basis[2][1]) < 1e-6);
    }

    // A camera at the origin looking down -z with a 90-degree reversed infinite projection: clip w is the distance and
    // the reversed depth 0.25 / distance.
    SceneCamera simple_camera()
    {
        Camera c {};
        const float view[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
        // Column-major: column 2 is (0, 0, 0, -1) and column 3 (0, 0, 0.25, 0).
        const float projection[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, -1, 0, 0, 0.25f, 0 };
        std::memcpy(c.world_to_view, view, sizeof view);
        std::memcpy(c.view_to_clip, projection, sizeof projection);
        return *scene_camera(c, false);
    }

    SceneDrawRecord record(bool stencil, uint32_t ref, bool depth_write, uint64_t first, uint64_t written)
    {
        SceneDrawRecord r;
        r.stencil = stencil;
        r.stencil_ref = ref;
        r.depth_write = depth_write;
        r.depth_func = 7;
        r.first_triangle = first;
        r.written = written;
        return r;
    }

    // What each present of a schedule did: "publish 6", "capture", "copy", "read", "done", "stop: why", joined by ' '.
    std::string present(SceneSchedule &s, const SceneFacts &given, FaceVerdict verdict)
    {
        SceneFacts facts = given;
        std::string out;
        if (s.wants_read())
        {
            facts.read = Judgement { verdict, {} };
            out += " read";
        }
        const SceneTick tick = scene_tick(s, facts);
        if (tick.publish_face != kFaceNone)
            out += " publish " + std::to_string(tick.publish_face);
        if (tick.capture_next_frame)
            out += " capture";
        if (tick.copy_at_present)
            out += " copy";
        if (tick.done)
            out += " done";
        if (tick.stop)
            out += " stop: " + *tick.stop;
        return out.empty() ? "-" : out.substr(1);
    }

    SceneRequest request(uint32_t faces, uint32_t settle)
    {
        EnhancementsSceneRequest raw { sizeof raw, faces, settle, 0, 0 };
        return *parse_scene_request(&raw);
    }
}

int main()
{
    {
        const std::optional<SceneRequest> defaults = parse_scene_request(std::array<EnhancementsSceneRequest, 1> { { { sizeof(EnhancementsSceneRequest), 0, 0, 0, 0 } } }.data());
        check("a zeroed request reads as all seven faces, settle 45, OBJ without raw bytes, 4000000 triangles a face",
            defaults && defaults->faces == 0x7F && defaults->settle == 45 && !defaults->raw && !defaults->ply && defaults->max_triangles == 4000000);
        EnhancementsSceneRequest raw { sizeof raw, 0xFFFFFF80u | 1u << ENHANCEMENTS_FACE_VIEW, 1, ENHANCEMENTS_SCENE_RAW | ENHANCEMENTS_SCENE_PLY | 0x100, 1000 };
        const auto clamped = parse_scene_request(&raw);
        check("unknown face bits drop, settle 1 clamps to 2, both flags read, 1000 triangles", clamped && clamped->faces == 0x40 && clamped->settle == 2
            && clamped->raw && clamped->ply && clamped->max_triangles == 1000);
        raw.settle = 5000;
        check("settle 5000 clamps to 600", parse_scene_request(&raw)->settle == 600);
        raw.faces = 0x80;
        check("only unknown face bits: invalid", !parse_scene_request(&raw));
        EnhancementsSceneRequest short_one { 8, 1u << ENHANCEMENTS_FACE_NEG_Y, 3, ENHANCEMENTS_SCENE_RAW, 7 };
        const auto cut = parse_scene_request(&short_one);
        check("a caller that knows only size and faces gets the defaults past them", cut && cut->faces == 0x08 && cut->settle == 45 && !cut->raw
            && cut->max_triangles == 4000000);
        short_one.size = 7;
        check("a size too short to hold faces, and a null pointer: invalid", !parse_scene_request(&short_one) && !parse_scene_request(nullptr));
        EnhancementsSceneRequest traced { sizeof traced, 0x7F, 0, ENHANCEMENTS_SCENE_TRACE | ENHANCEMENTS_SCENE_NO_MESHES | ENHANCEMENTS_SCENE_GBUFFER, 0 };
        const auto t = parse_scene_request(&traced);
        check("the trace, no-meshes and G-buffer flags read", t && t->trace && t->no_meshes && t->gbuffer && !t->release && !t->self_test() && t->faces == 0x7F);
        traced.faces = 0;
        const auto self = parse_scene_request(&traced);
        check("a trace request with no faces is the self-test scene, and without the trace flag all seven faces", self && self->self_test()
            && self->faces == 0 && start_scene(*self).order.empty() && parse_scene_request(std::array<EnhancementsSceneRequest, 1> {
                { { sizeof(EnhancementsSceneRequest), 0, 0, ENHANCEMENTS_SCENE_GBUFFER, 0 } } }.data())->faces == 0x7F);
        traced.flags = ENHANCEMENTS_SCENE_TRACE;
        traced.faces = 0x80;
        check("a trace request naming only unknown faces is invalid", !parse_scene_request(&traced));
        EnhancementsSceneRequest release { sizeof release, 0x80, 0, ENHANCEMENTS_SCENE_RELEASE, 0 };
        check("a release ignores the faces", parse_scene_request(&release) && parse_scene_request(&release)->release);
    }

    check_transform({ "capture-20261010-111824",
        { { 0.269, 219.656, -10.805 }, { -34.868, 188.629, -135.816 }, { 0.941, 224.052, 5.633 }, { 49.813, 167.398, -267.738 }, { -0.328, 226.690, 23.534 } } });
    check_transform({ "capture-20261009-214859",
        { { 22.193, 250.408, -45.744 }, { 23.063, 217.695, -175.182 }, { 18.287, 255.024, -29.825 }, { 140.986, 194.750, -278.103 }, { 12.104, 257.901, -13.017 } } });
    {
        const SceneCamera camera = simple_camera();
        check("a clip point at infinity (w' = 0) gives nothing", !clip_to_world(camera, { 1, 0, 0, 0 }));
        check("a singular matrix does not invert", !invert(Mat4d {}));
    }
    for (const char *capture : { "capture-20261010-111824", "capture-20261009-214859" })
    {
        const SunCbuffers bytes = load_sun(capture);
        std::string why;
        const std::optional<SceneCamera> alone = scene_camera(bytes, why);
        const SceneCamera parsed = *scene_camera(parse_sun_shadows(bytes).constants);
        check((std::string(capture) + ": the camera from b1 and b2 alone is the parsed constants' camera, to the bit").c_str(),
            alone && why.empty() && alone->clip_to_world == parsed.clip_to_world && alone->position == parsed.position);
    }
    {
        // Cascade 2's sphere shrunk below cascade 1's, in rows 0 to 7 and in row 8 that repeats them: b3 starts at byte 1312.
        SunCbuffers bytes = load_sun("capture-20261010-111824");
        float radius2;
        std::memcpy(&radius2, bytes.data() + 1312 + 1 * 16 + 12, sizeof radius2);
        radius2 /= 2;
        std::memcpy(bytes.data() + 1312 + 2 * 16 + 12, &radius2, sizeof radius2);
        std::memcpy(bytes.data() + 1312 + 8 * 16 + 2 * 4, &radius2, sizeof radius2);
        std::string why;
        const std::optional<SceneCamera> camera = scene_camera(bytes, why);
        check("cascades that fail radii-order for the sun pass still give the game's camera", parse_sun_shadows(bytes).check == SunCheck::radii_order
            && camera && camera->source == SceneCamera::Source::game && why.empty());
        SunCbuffers skewed = bytes;
        const float two = 2;
        std::memcpy(skewed.data() + 960 + 18 * 16, &two, sizeof two);
        SunCbuffers infinite = bytes;
        const float nan = NAN;
        std::memcpy(infinite.data() + 768 + 5 * 16 + 4, &nan, sizeof nan);
        std::string skewed_why, infinite_why;
        check("a right vector of length 2: no camera, basis; a NaN in b1 row 5: no camera, not-finite",
            !scene_camera(skewed, skewed_why) && skewed_why == "basis" && !scene_camera(infinite, infinite_why) && infinite_why == "not-finite");
    }

    {
        // b2's basis turned onto each face: the rotation that takes b2's forward and up to the face's.
        const SunParse parse = parse_sun_shadows(load_sun("capture-20261010-111824"));
        const SceneCamera game = *scene_camera(parse.constants);
        const auto cross = [](const Vec3d &a, const Vec3d &b) { return Vec3d { a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0] }; };
        bool accepted = true, rejected = true;
        for (uint32_t face = 0; face < kFaceView; face++)
        {
            // Unity's left-handed basis: right = up x forward.
            const Vec3d to_forward = kFaces[face].forward, to_up = kFaces[face].up, to_right = cross(to_up, to_forward);
            SceneCamera turned = game;
            for (int i = 0; i < 3; i++)
            {
                Vec3d v {};
                for (int k = 0; k < 3; k++)
                    v[k] = to_right[k] * (game.basis[0][0] * game.basis[i][0] + game.basis[0][1] * game.basis[i][1] + game.basis[0][2] * game.basis[i][2])
                        + to_up[k] * (game.basis[1][0] * game.basis[i][0] + game.basis[1][1] * game.basis[i][1] + game.basis[1][2] * game.basis[i][2])
                        + to_forward[k] * (game.basis[2][0] * game.basis[i][0] + game.basis[2][1] * game.basis[i][1] + game.basis[2][2] * game.basis[i][2]);
                turned.basis[i] = v;
            }
            accepted &= camera_faces(turned, face) && !camera_faces(turned, (face + 2) % kFaceView);
            // 2 degrees of pitch: forward turns toward up.
            const double c = std::cos(2 * M_PI / 180), s = std::sin(2 * M_PI / 180);
            SceneCamera pitched = turned;
            for (int k = 0; k < 3; k++)
            {
                pitched.basis[2][k] = c * turned.basis[2][k] + s * turned.basis[1][k];
                pitched.basis[1][k] = c * turned.basis[1][k] - s * turned.basis[2][k];
            }
            rejected &= !camera_faces(pitched, face);
        }
        check("111824's camera turned onto each of the six faces shows that face and not another", accepted);
        check("the same turned 2 degrees further does not", rejected);
        check("any camera shows the view", camera_faces(game, kFaceView));
    }

    {
        // Six corners, two triangles, against a 4x4 depth buffer at 10 m: the first triangle is world geometry (stencil
        // 128), the second the depth prepass.
        const SceneCamera camera = simple_camera();
        const std::vector<float> depth(16, 0.025f);
        const auto at = [](float column, float row, float w) {
            const float ndc_x = (column + 0.5f) / 4 * 2 - 1, ndc_y = 1 - (row + 0.5f) / 4 * 2;
            return std::array<float, 4> { ndc_x * w, ndc_y * w, 0.25f, w };
        };
        std::vector<float> clip;
        for (const std::array<float, 4> &v : { at(0, 0, 10), at(1, 1, 9), at(2, 2, 12), at(1, 2, -5), at(5.5f, 0, 10), at(3, 3, 10.05f) })
            clip.insert(clip.end(), v.begin(), v.end());
        const std::vector<SceneDrawRecord> records = { record(true, 128, false, 0, 1), record(false, 0, true, 1, 1) };
        const DepthCheck d = check_depth(camera, records, clip, { 4, 4 }, depth);
        check("stencil 128: 3 tested, 1 matched at 10 m, 1 in front at 9 m, 1 behind at 12 m",
            d.classes[static_cast<size_t>(DrawClass::s128)] == DepthCounts { 3, 1, 1, 1 });
        check("the prepass: 1 tested and matched at 10.05 m; 2 off screen, one behind the camera and one right of the picture",
            d.classes[static_cast<size_t>(DrawClass::pre)] == DepthCounts { 1, 1, 0, 0 } && d.off_screen == 2);
        // The right column at 20 m: a corner on the silhouette between the two depths matches either.
        std::vector<float> step = depth;
        for (int row = 0; row < 4; row++)
            step[row * 4 + 3] = 0.0125f;
        std::vector<float> edge;
        for (const std::array<float, 4> &v : { at(2, 0, 20), at(2, 1, 10), at(1, 1, 20) })
            edge.insert(edge.end(), v.begin(), v.end());
        const DepthCheck e = check_depth(camera, { record(true, 128, false, 0, 1) }, edge, { 4, 4 }, step);
        check("beside a depth step, a corner at 20 m and one at 10 m both match, and one at 20 m two pixels from the step is behind",
            e.classes[static_cast<size_t>(DrawClass::s128)] == DepthCounts { 3, 2, 0, 1 });
    }

    {
        // A frame's replays placed one after another in a buffer of 100,000 corners.
        const auto call = [](ReplayKind kind, uint32_t count, uint32_t instances, bool points) {
            SceneDrawRecord r;
            r.kind = kind;
            r.count = count;
            r.instances = instances;
            r.topology = points ? 4 : 35;
            r.points = points;
            r.ds = points ? 0 : 0x2adc971cc2058b7aull;
            return r;
        };
        std::vector<SceneDrawRecord> frame = { call(ReplayKind::indexed, 6, 2, true), call(ReplayKind::indirect_indexed, 0, 0, true),
            call(ReplayKind::indexed, 30, 1, false), call(ReplayKind::indexed, 3000, 1, true), call(ReplayKind::draw, 300, 1, true) };
        frame[1].args_offset = 20;
        StreamLayout layout;
        layout.capacity = 100000;
        ReserveHints hints;
        std::vector<bool> placed;
        for (SceneDrawRecord &r : frame)
            placed.push_back(place_draw(layout, r, hints));
        check("layout: a direct point replay reserves count x instances corners, an indirect or tessellated one 49,152",
            frame[0].offset == 0 && frame[0].reserved == 12 && frame[1].offset == 12 && frame[1].reserved == 49152 && frame[2].offset == 49164
                && frame[2].reserved == 49152);
        check("layout: 3,000 corners past the buffer's end are skipped as buffer-full, and the 300 after them still fit",
            !placed[3] && frame[3].skip == DrawSkip::buffer_full && placed[4] && frame[4].offset == 98316 && layout.used == 98616
                && layout.wanted == 101616);
        StreamLayout wide;
        wide.capacity = 100;
        SceneDrawRecord alpha = call(ReplayKind::indexed, 6, 1, true), after = call(ReplayKind::indexed, 3, 1, true);
        alpha.stride = 36;
        check("layout: 6 corners of 36 bytes, a position and 5 floats of an alpha program, take 14 units of 16 bytes, and the next draw starts after them",
            place_draw(wide, alpha, hints) && place_draw(wide, after, hints) && alpha.offset == 0 && alpha.reserved == 6 && after.offset == 14
                && wide.used == 17);
        hints[reserve_key(frame[1])] = 75776;
        hints[reserve_key(frame[2])] = 1024;
        SceneDrawRecord other_slot = frame[1];
        other_slot.args_offset = 40;
        check("a hint above 49,152 corners raises an indirect draw's reservation, one below does not, and another argument slot has its own",
            corners_to_reserve(frame[1], hints) == 75776 && corners_to_reserve(frame[2], hints) == 49152 && corners_to_reserve(other_slot, hints) == 49152);
    }
    {
        // 32 corners of buffer, cleared to the marker, then written as five replays left them. Corner i is (i, i + 0.5,
        // 0.25, 10).
        std::vector<float> buffer(32 * 4);
        for (size_t i = 0; i < buffer.size(); i++)
            std::memcpy(&buffer[i], &kUnwrittenBits, sizeof(float));
        const auto write = [&buffer](uint64_t corner) {
            const float c[4] = { float(corner), corner + 0.5f, 0.25f, 10 };
            std::memcpy(&buffer[corner * 4], c, sizeof c);
        };
        const auto replay = [](ReplayKind kind, uint32_t count, uint64_t offset, uint64_t reserved, uint64_t corners) {
            SceneDrawRecord r;
            r.kind = kind;
            r.count = count;
            r.instances = 2;
            r.topology = 4;
            r.points = true;
            r.offset = offset;
            r.reserved = reserved;
            r.corners = r.corners_needed = corners;
            return r;
        };
        // An indirect draw with room for 6 corners that wrote 60,000: its first 6 are in place, its 7th to 9th were
        // overwritten by the next draw's 3, and the rest landed past them.
        std::vector<SceneDrawRecord> records = { replay(ReplayKind::indirect_indexed, 0, 0, 6, 60000), replay(ReplayKind::indirect_indexed, 0, 6, 3, 3),
            // A direct draw that said it wrote 6 corners but wrote 2, as the Deck's DXVK did for triangles: 4 unwritten.
            replay(ReplayKind::indexed, 3, 9, 6, 6),
            // 2 instances of 4 points: one triangle each, the 4th corner of each left out.
            replay(ReplayKind::indexed, 4, 15, 8, 8),
            // A corner a vertex shader culled by writing zeros is written.
            replay(ReplayKind::indexed, 3, 23, 6, 6) };
        records[0].args_offset = 0;
        records[1].args_offset = 20;
        SceneDrawRecord skipped;
        skipped.skip = DrawSkip::buffer_full;
        records.push_back(skipped);
        for (uint64_t corner : { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28 })
            write(corner);
        std::memset(&buffer[25 * 4], 0, 16);
        ReserveHints hints;
        const StreamRead read = unpack_stream(records, buffer.data(), 32, hints);
        std::vector<float> want;
        for (uint64_t corner : { 6, 7, 8, 15, 16, 17, 19, 20, 21, 23, 24, 25, 26, 27, 28 })
        {
            const float c[4] = { float(corner), corner + 0.5f, 0.25f, 10 };
            want.insert(want.end(), c, c + 4);
        }
        std::memset(&want[11 * 4], 0, 16);
        check("unpack: the overrun draw is left out and its hint becomes 1.25 x 60,000 corners rounded up to 1,024; the next draw's own 3 corners stay",
            !streamed(records[0]) && read.overran == 1 && hints[reserve_key(records[0])] == 75776 && corners_to_reserve(records[0], hints) == 75776
                && streamed(records[1]) && records[1].written == 1);
        check("unpack: 4 of a draw's 6 corners never written are counted and leave it out", records[2].unwritten == 4 && !streamed(records[2])
            && read.unwritten == 1 && records[2].written == 2);
        check("unpack: 2 instances of 4 points give 2 triangles without the 4th corner of each, and a zero corner counts as written",
            records[3].written == 2 && records[4].unwritten == 0 && records[4].written == 2 && read.clip == want);
        check("unpack: first triangles count only streamed draws, a skipped draw included",
            records[0].first_triangle == 0 && records[1].first_triangle == 0 && records[2].first_triangle == 1 && records[3].first_triangle == 1
                && records[4].first_triangle == 3 && records[5].first_triangle == 5 && records[0].written == 20000);
    }

    {
        // A draw of 24 bytes a corner, its position and two floats (u, v), with 2 triangles at unit 2 of a buffer of 16 units
        // cleared to the marker, its third corner's floats left as the marker: only the positions decide what is written.
        std::vector<float> buffer(16 * 4);
        for (size_t i = 0; i < buffer.size(); i++)
            std::memcpy(&buffer[i], &kUnwrittenBits, sizeof(float));
        float *base = &buffer[2 * 4];
        for (uint64_t corner = 0; corner < 6; corner++)
        {
            const float c[6] = { float(corner), 1, 2, 3, corner * 0.1f, corner * 0.2f };
            std::memcpy(base + corner * 6, c, corner == 2 ? 16 : sizeof c);
        }
        SceneDrawRecord r;
        r.kind = ReplayKind::indexed;
        r.count = 6;
        r.instances = 1;
        r.topology = 4;
        r.points = true;
        r.stride = 24;
        r.offset = 2;
        r.reserved = 6;
        r.corners = r.corners_needed = 6;
        SceneDrawRecord before = r;
        before.stride = 16;
        before.offset = 0;
        before.reserved = before.corners = before.corners_needed = 0;
        std::vector<SceneDrawRecord> records = { before, r };
        ReserveHints hints;
        const StreamRead read = unpack_stream(records, buffer.data(), 16, hints);
        std::vector<float> clip, attributes;
        for (uint64_t corner = 0; corner < 6; corner++)
        {
            const float p[4] = { float(corner), 1, 2, 3 };
            clip.insert(clip.end(), p, p + 4);
            attributes.push_back(corner * 0.1f);
            attributes.push_back(corner * 0.2f);
        }
        std::memcpy(&attributes[4], &kUnwrittenBits, 4);
        std::memcpy(&attributes[5], &kUnwrittenBits, 4);
        check("unpack at 24 bytes a corner: positions to clip, the two floats after each to attributes, the marker checked on positions only",
            streamed(records[1]) && records[1].written == 2 && records[1].unwritten == 0 && read.clip == clip && read.attributes.size() == 12
                && std::memcmp(read.attributes.data(), attributes.data(), 48) == 0 && records[1].first_attribute == 0 && records[1].first_triangle == 0);
    }

    {
        SceneDrawRecord r = record(true, 137, false, 0, 0);
        r.depth_func = 3;
        SceneDrawRecord pre = record(false, 0, true, 0, 0);
        check("draw kinds: the prepass, equal depth, main", std::string(draw_kind(pre)) == "pre" && std::string(draw_kind(r)) == "equal"
            && std::string(draw_kind(record(true, 128, false, 0, 0))) == "main");
        check("draw classes: pre, s137, other for stencil 200 and for stencil off without depth write", draw_class(pre) == DrawClass::pre
            && draw_class(r) == DrawClass::s137 && draw_class(record(true, 200, false, 0, 0)) == DrawClass::other
            && draw_class(record(false, 0, false, 0, 0)) == DrawClass::other);
        r.kind = ReplayKind::indexed;
        r.topology = 4;
        r.count = 4902;
        r.instances = 2;
        SceneDrawRecord strip = r;
        strip.topology = 5;
        strip.count = 10;
        SceneDrawRecord indirect = r;
        indirect.kind = ReplayKind::indirect_indexed;
        SceneDrawRecord patches = r;
        patches.topology = 35;
        check("expected triangles: 4902 indices x 2 is 3268, a 10-index strip 8 a time, none for indirect or patches",
            expected_triangles(r) == uint64_t(3268) && expected_triangles(strip) == uint64_t(16) && !expected_triangles(indirect) && !expected_triangles(patches));
    }

    {
        const SceneCamera camera = simple_camera();
        const std::vector<SceneDrawRecord> drawn = { record(true, 128, false, 0, 4) };
        SceneDrawRecord skipped = record(true, 128, false, 0, 0);
        skipped.skip = DrawSkip::no_bytecode;
        const std::vector<SceneDrawRecord> none_replayed = { skipped, skipped, skipped };
        check("a frame without the G-buffer step: no_gbuffer", judge_face(kFaceView, drawn, false, false, true, camera, std::nullopt).verdict == FaceVerdict::no_gbuffer);
        const Judgement nothing = judge_face(kFaceView, none_replayed, true, false, true, camera, std::nullopt);
        check("every draw skipped: nothing, naming the commonest reason", nothing.verdict == FaceVerdict::nothing
            && nothing.why == "3 of 3 draws were skipped for no-bytecode");
        check("no camera: no_camera", judge_face(kFaceView, drawn, true, false, true, std::nullopt, std::nullopt).verdict == FaceVerdict::no_camera);
        check("a camera looking down -z for +X: wrong_face, and for -Z: ok", judge_face(ENHANCEMENTS_FACE_POS_X, drawn, true, false, true, camera, std::nullopt).verdict
            == FaceVerdict::wrong_face && judge_face(ENHANCEMENTS_FACE_NEG_Z, drawn, true, false, true, camera, std::nullopt).verdict == FaceVerdict::ok);
        const Judgement moved = judge_face(kFaceView, drawn, true, false, true, camera, Vec3d { 0, 0.25, 0 });
        check("0.25 m from the first saved face's camera: moved", moved.verdict == FaceVerdict::moved && moved.why == "it was 0.250 m from the first saved face's");
        check("0.5 mm from it: ok", judge_face(kFaceView, drawn, true, false, true, camera, Vec3d { 0, 0.0005, 0 }).verdict == FaceVerdict::ok);
        check("an overflow that can grow: overflow; at the largest buffer: ok", judge_face(kFaceView, drawn, true, true, true, camera, std::nullopt).verdict
            == FaceVerdict::overflow && judge_face(kFaceView, drawn, true, true, false, camera, std::nullopt).verdict == FaceVerdict::ok);
    }

    {
        SceneSchedule s = start_scene(request(0x7F, 45));
        check("all seven faces go in order view, +X, -X, +Y, -Y, +Z, -Z", s.order == std::vector<uint32_t> { 6, 0, 1, 2, 3, 4, 5 });
        std::vector<std::string> ticks;
        for (uint32_t p = 0; p < 300 && (ticks.empty() || ticks.back().find("done") == std::string::npos); p++)
            ticks.push_back(present(s, {}, FaceVerdict::ok));
        check("the view: published at 0, captured at 2, copied at 3, read at 5, where +X is published",
            ticks[0] == "publish 6" && ticks[1] == "publish 6" && ticks[2] == "publish 6 capture" && ticks[3] == "publish 6 copy" && ticks[4] == "publish 6"
                && ticks[5] == "read publish 0");
        check("+X: captured 45 presents after its publish, at 50, copied at 51, read at 53, where -X is published",
            ticks[49] == "publish 0" && ticks[50] == "publish 0 capture" && ticks[51] == "publish 0 copy" && ticks[52] == "publish 0"
                && ticks[53] == "read publish 1");
        check("-Z, the last, is captured at 290 and read at 293, which ends the capture with 7 saved", ticks.size() == 294 && ticks[290] == "publish 5 capture"
            && ticks[293] == "read done" && s.saved == 7 && present(s, {}, FaceVerdict::ok) == "done");
    }
    {
        SceneSchedule s = start_scene(request(1u << ENHANCEMENTS_FACE_VIEW, 45));
        std::vector<std::string> ticks;
        for (int p = 0; p < 6; p++)
            ticks.push_back(present(s, {}, FaceVerdict::ok));
        check("the view alone is done at present 5", ticks == std::vector<std::string> { "publish 6", "publish 6", "publish 6 capture", "publish 6 copy", "publish 6",
            "read done" });
    }
    {
        SceneSchedule s = start_scene(request(1u << ENHANCEMENTS_FACE_VIEW, 45));
        std::vector<std::string> ticks;
        for (int p = 0; p < 9; p++)
            ticks.push_back(present(s, {}, p < 8 ? FaceVerdict::not_ready : FaceVerdict::ok));
        check("not ready at 5, 6 and 7 reads again each present, and ok at 8 is done", ticks[5] == "read publish 6" && ticks[6] == "read publish 6"
            && ticks[7] == "read publish 6" && ticks[8] == "read done");
    }
    {
        SceneSchedule s = start_scene(request(1u << ENHANCEMENTS_FACE_POS_X, 2));
        std::vector<std::string> ticks;
        for (int p = 0; p < 27; p++)
            ticks.push_back(present(s, {}, FaceVerdict::wrong_face));
        check("+X facing elsewhere is captured again at once: at 2, then 5, 8 and on to 23, 8 captures",
            ticks[2] == "publish 0 capture" && ticks[5] == "read publish 0 capture" && ticks[23] == "read publish 0 capture" && ticks[24] == "publish 0 copy");
        check("the 8th read, at 26, stops and names the free camera",
            ticks[26] == "read stop: the camera did not turn to +X: is CameraTools' free camera on?");
    }
    {
        SceneSchedule s = start_scene(request(1u << ENHANCEMENTS_FACE_POS_X, 2));
        std::vector<std::string> ticks;
        for (int p = 0; p < 27; p++)
            ticks.push_back(present(s, {}, FaceVerdict::no_gbuffer));
        check("8 captures without a G-buffer stop with the verdict", ticks[26] == "read stop: 8 captures of +X failed, the last because the frame had no G-buffer");
    }
    {
        SceneSchedule s = start_scene(request(1u << ENHANCEMENTS_FACE_POS_X, 2));
        SceneFacts held;
        held.backlog = 2;
        std::vector<std::string> ticks;
        for (int p = 0; p < 12; p++)
            ticks.push_back(present(s, p < 10 ? held : SceneFacts {}, FaceVerdict::ok));
        check("a backlog of 2 faces holds the capture but not the publish, and the capture comes when it drops", ticks[0] == "publish 0" && ticks[9] == "publish 0"
            && ticks[10] == "publish 0 capture" && ticks[11] == "publish 0 copy");
    }
    {
        SceneSchedule s = start_scene(request(1u << ENHANCEMENTS_FACE_POS_X, 45));
        SceneFacts held;
        held.backlog = 2;
        std::string last;
        uint32_t p = 0;
        for (; p < 1000 && last.find("stop") == std::string::npos; p++)
            last = present(s, held, FaceVerdict::ok);
        check("a face not saved 900 presents after its publish stops the capture", p == 901 && last == "stop: +X was not saved within 900 presents");
    }
    {
        SceneSchedule s = start_scene(request(0x7F, 45));
        std::vector<std::string> ticks;
        for (int p = 0; p < 6; p++)
            ticks.push_back(present(s, {}, FaceVerdict::moved));
        check("moved stops at once", ticks[5] == "read stop: the camera moved at view" && present(s, {}, FaceVerdict::ok) == "done");
        SceneSchedule t = start_scene(request(0x7F, 45));
        SceneFacts stop;
        stop.stop = "the D3D11 device went away";
        check("a stop from the shell ends the capture", present(t, stop, FaceVerdict::ok) == "stop: the D3D11 device went away");
    }
    std::printf("%s: %d failed\n", __FILE__, failures);
    return failures == 0 ? 0 : 1;
}
