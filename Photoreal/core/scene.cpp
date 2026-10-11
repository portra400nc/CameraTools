#include "scene.h"
#include "../photoreal.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>

namespace photoreal
{
    namespace
    {
        static_assert(PHOTOREAL_FACE_COUNT == kFaceCount && PHOTOREAL_FACE_VIEW == kFaceView && PHOTOREAL_FACE_NONE == kFaceNone
            && PHOTOREAL_FACE_POS_Y == 2 && PHOTOREAL_FACE_NEG_Z == 5);
        static_assert(offsetof(PhotorealSceneRequest, faces) == 4 && sizeof(PhotorealSceneRequest) == 20);

        constexpr uint32_t kAllFaces = (1u << kFaceCount) - 1;

        // D3D11_PRIMITIVE_TOPOLOGY values expected_triangles knows.
        constexpr uint32_t kTriangleList = 4, kTriangleStrip = 5;
        constexpr uint32_t kComparisonEqual = 3;

        double dot(const Vec3d &a, const Vec3d &b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
        double distance(const Vec3d &a, const Vec3d &b)
        {
            const Vec3d d = { a[0] - b[0], a[1] - b[1], a[2] - b[2] };
            return std::sqrt(dot(d, d));
        }

        Mat4d multiply(const Mat4d &a, const Mat4d &b)
        {
            Mat4d out {};
            for (int r = 0; r < 4; r++)
                for (int c = 0; c < 4; c++)
                    for (int k = 0; k < 4; k++)
                        out[r * 4 + c] += a[r * 4 + k] * b[k * 4 + c];
            return out;
        }

        // Unity's column-major memory order into ours.
        Mat4d from_unity(const float m[16])
        {
            Mat4d out {};
            for (int r = 0; r < 4; r++)
                for (int c = 0; c < 4; c++)
                    out[r * 4 + c] = m[c * 4 + r];
            return out;
        }

        bool finite(const Mat4d &m)
        {
            return std::all_of(m.begin(), m.end(), [](double v) { return std::isfinite(v); });
        }

        Vec3d widen(const Vec3 &v) { return { v[0], v[1], v[2] }; }
    }

    const std::array<FaceSpec, kFaceCount> kFaces = { {
        { "pos-x", "+X", { 1, 0, 0 }, { 0, 1, 0 } },
        { "neg-x", "-X", { -1, 0, 0 }, { 0, 1, 0 } },
        { "pos-y", "+Y", { 0, 1, 0 }, { 0, 0, -1 } },
        { "neg-y", "-Y", { 0, -1, 0 }, { 0, 0, 1 } },
        { "pos-z", "+Z", { 0, 0, 1 }, { 0, 1, 0 } },
        { "neg-z", "-Z", { 0, 0, -1 }, { 0, 1, 0 } },
        { "view", "view", { 0, 0, 0 }, { 0, 0, 0 } },
    } };

    const std::array<const char *, 2> kCameraSourceNames = { "game", "pushed" };
    const std::array<const char *, static_cast<size_t>(ReplayKind::count)> kReplayKindNames = { "draw", "indexed", "indirect", "indirect-indexed" };
    const std::array<const char *, static_cast<size_t>(DrawSkip::count)> kDrawSkipNames = {
        "-", "no-bytecode", "gs-bound", "no-position", "game-so", "query-pool", "gs-create-failed", "topology",
    };
    const std::array<const char *, static_cast<size_t>(DrawClass::count)> kDrawClassNames = {
        "pre", "s128", "s129", "s133", "s136", "s137", "s144", "s165", "other",
    };
    const std::array<const char *, static_cast<size_t>(FaceVerdict::count)> kFaceVerdictNames = {
        "ok", "not ready", "the camera faced elsewhere", "the camera moved", "the frame had no G-buffer", "the stream output overflowed",
        "no G-buffer draw could be replayed", "no camera", "its copies could not be read",
    };

    std::optional<SceneRequest> parse_scene_request(const PhotorealSceneRequest *raw)
    {
        if (raw == nullptr || raw->size < offsetof(PhotorealSceneRequest, faces) + sizeof raw->faces)
            return std::nullopt;
        PhotorealSceneRequest copy {};
        std::memcpy(&copy, raw, std::min<size_t>(raw->size, sizeof copy));
        const auto knows = [&copy](size_t offset, size_t bytes) { return copy.size >= offset + bytes; };
        SceneRequest r;
        if (copy.faces != 0)
            r.faces = copy.faces & kAllFaces;
        if (r.faces == 0)
            return std::nullopt;
        if (knows(offsetof(PhotorealSceneRequest, settle), sizeof copy.settle) && copy.settle != 0)
            r.settle = std::clamp<uint32_t>(copy.settle, 2, 600);
        if (knows(offsetof(PhotorealSceneRequest, flags), sizeof copy.flags))
        {
            r.raw = (copy.flags & PHOTOREAL_SCENE_RAW) != 0;
            r.ply = (copy.flags & PHOTOREAL_SCENE_PLY) != 0;
        }
        if (knows(offsetof(PhotorealSceneRequest, max_triangles), sizeof copy.max_triangles) && copy.max_triangles != 0)
            r.max_triangles = copy.max_triangles;
        return r;
    }

    std::optional<SceneCamera> scene_camera(const SunShadowConstants &game)
    {
        // GL clip from the vertex shaders' D3D clip: y back up, and z from reversed 0..1 to GL's -1..1, z_gl = w - 2z.
        const Mat4d from_d3d = { 1, 0, 0, 0, 0, -1, 0, 0, 0, 0, -2, 1, 0, 0, 0, 1 };
        // b2 rows 10 to 13 are the inverse projection's columns: view = ndc.x * [0] + ndc.y * [1] + ndc.z * [2] + ndc.w * [3].
        Mat4d inverse_projection {};
        for (int column = 0; column < 4; column++)
            for (int row = 0; row < 4; row++)
                inverse_projection[row * 4 + column] = game.inverse_projection[column][row];
        SceneCamera camera;
        camera.source = SceneCamera::Source::game;
        camera.position = widen(game.camera);
        for (int i = 0; i < 3; i++)
            camera.basis[i] = widen(game.basis[i]);
        // Unity's view space looks down -z, so view z runs against the camera's forward.
        const Vec3d &right = camera.basis[0], &up = camera.basis[1], &forward = camera.basis[2];
        Mat4d view_to_world = {};
        for (int k = 0; k < 3; k++)
        {
            view_to_world[k * 4 + 0] = right[k];
            view_to_world[k * 4 + 1] = up[k];
            view_to_world[k * 4 + 2] = -forward[k];
            view_to_world[k * 4 + 3] = camera.position[k];
        }
        view_to_world[15] = 1;
        camera.clip_to_world = multiply(view_to_world, multiply(inverse_projection, from_d3d));
        if (!finite(camera.clip_to_world))
            return std::nullopt;
        return camera;
    }

    std::optional<SceneCamera> scene_camera(const Camera &pushed, bool flip)
    {
        const Mat4d view = from_unity(pushed.world_to_view);
        Mat4d to_clip = multiply(from_unity(pushed.view_to_clip), view);
        if (flip)
            for (int c = 0; c < 4; c++)
                to_clip[4 + c] = -to_clip[4 + c];
        const std::optional<Mat4d> inverse = invert(to_clip);
        if (!inverse || !finite(*inverse) || !finite(view))
            return std::nullopt;
        SceneCamera camera;
        camera.source = SceneCamera::Source::pushed;
        camera.clip_to_world = *inverse;
        // world_to_view = [R t]: the rows of R are right, up and minus forward, and the camera sits at -R^T t.
        for (int k = 0; k < 3; k++)
        {
            camera.basis[0][k] = view[0 * 4 + k];
            camera.basis[1][k] = view[1 * 4 + k];
            camera.basis[2][k] = -view[2 * 4 + k];
            camera.position[k] = -(view[0 * 4 + k] * view[3] + view[1 * 4 + k] * view[7] + view[2 * 4 + k] * view[11]);
        }
        return camera;
    }

    std::optional<Vec3d> clip_to_world(const SceneCamera &camera, const std::array<float, 4> &clip)
    {
        const Mat4d &m = camera.clip_to_world;
        double h[4] = {};
        for (int r = 0; r < 4; r++)
            for (int c = 0; c < 4; c++)
                h[r] += m[r * 4 + c] * clip[c];
        if (!(std::fabs(h[3]) >= 1e-12))
            return std::nullopt;
        return Vec3d { h[0] / h[3], h[1] / h[3], h[2] / h[3] };
    }

    std::optional<Mat4d> invert(const Mat4d &m)
    {
        // Gauss-Jordan with partial pivoting, in double.
        Mat4d a = m, out = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
        for (int column = 0; column < 4; column++)
        {
            int pivot = column;
            for (int r = column + 1; r < 4; r++)
                if (std::fabs(a[r * 4 + column]) > std::fabs(a[pivot * 4 + column]))
                    pivot = r;
            if (!(std::fabs(a[pivot * 4 + column]) > 1e-300))
                return std::nullopt;
            for (int c = 0; c < 4; c++)
            {
                std::swap(a[column * 4 + c], a[pivot * 4 + c]);
                std::swap(out[column * 4 + c], out[pivot * 4 + c]);
            }
            const double scale = 1 / a[column * 4 + column];
            for (int c = 0; c < 4; c++)
            {
                a[column * 4 + c] *= scale;
                out[column * 4 + c] *= scale;
            }
            for (int r = 0; r < 4; r++)
            {
                if (r == column)
                    continue;
                const double factor = a[r * 4 + column];
                for (int c = 0; c < 4; c++)
                {
                    a[r * 4 + c] -= factor * a[column * 4 + c];
                    out[r * 4 + c] -= factor * out[column * 4 + c];
                }
            }
        }
        return out;
    }

    bool camera_faces(const SceneCamera &camera, uint32_t face)
    {
        if (face >= kFaceView)
            return true;
        return dot(camera.basis[2], kFaces[face].forward) >= 0.9995 && dot(camera.basis[1], kFaces[face].up) >= 0.999;
    }

    const char *draw_kind(const SceneDrawRecord &record)
    {
        if (!record.stencil && record.depth_write)
            return "pre";
        return record.depth_func == kComparisonEqual ? "equal" : "main";
    }

    DrawClass draw_class(const SceneDrawRecord &record)
    {
        if (!record.stencil)
            return record.depth_write ? DrawClass::pre : DrawClass::other;
        switch (record.stencil_ref)
        {
        case 128: return DrawClass::s128;
        case 129: return DrawClass::s129;
        case 133: return DrawClass::s133;
        case 136: return DrawClass::s136;
        case 137: return DrawClass::s137;
        case 144: return DrawClass::s144;
        case 165: return DrawClass::s165;
        default: return DrawClass::other;
        }
    }

    std::optional<uint64_t> expected_triangles(const SceneDrawRecord &record)
    {
        if (record.kind != ReplayKind::draw && record.kind != ReplayKind::indexed)
            return std::nullopt;
        if (record.topology == kTriangleList)
            return uint64_t(record.count / 3) * record.instances;
        if (record.topology == kTriangleStrip)
            return uint64_t(record.count >= 3 ? record.count - 2 : 0) * record.instances;
        return std::nullopt;
    }

    DepthCheck check_depth(const SceneCamera &camera, const std::vector<SceneDrawRecord> &records, const std::vector<float> &clip, Size size,
        const std::vector<float> &depth)
    {
        DepthCheck check;
        if (size.width == 0 || size.height == 0 || depth.size() < size_t(size.width) * size.height)
            return check;
        // Each pixel's depth at its center, placed by the same camera, in meters along its forward axis; NaN where the
        // camera places it at infinity, which then matches nothing.
        const Vec3d &forward = camera.basis[2];
        std::vector<float> meters(size_t(size.width) * size.height);
        for (uint32_t py = 0; py < size.height; py++)
            for (uint32_t px = 0; px < size.width; px++)
            {
                const float ndc_x = (px + 0.5f) / size.width * 2 - 1, ndc_y = 1 - (py + 0.5f) / size.height * 2;
                const size_t at = size_t(py) * size.width + px;
                const std::optional<Vec3d> surface = clip_to_world(camera, { ndc_x, ndc_y, depth[at], 1 });
                meters[at] = surface ? float(dot({ (*surface)[0] - camera.position[0], (*surface)[1] - camera.position[1], (*surface)[2] - camera.position[2] },
                    forward)) : NAN;
            }
        for (const SceneDrawRecord &record : records)
        {
            DepthCounts &counts = check.classes[static_cast<size_t>(draw_class(record))];
            const size_t first = size_t(record.first_triangle) * 12, end = first + size_t(record.written) * 12;
            for (size_t at = first; at < end && at + 4 <= clip.size(); at += 4)
            {
                const float x = clip[at], y = clip[at + 1], w = clip[at + 3];
                const double column = (x / w * 0.5 + 0.5) * size.width, row = (0.5 - 0.5 * y / w) * size.height;
                if (!(w > 0) || !(column >= 0 && column < size.width && row >= 0 && row < size.height))
                {
                    check.off_screen++;
                    continue;
                }
                // A corner on a silhouette, or on a surface seen edge-on, lies between pixel centers whose depths differ by
                // more than the tolerance, so it is held against the range of the 3x3 pixels around it.
                const uint32_t px = static_cast<uint32_t>(column), py = static_cast<uint32_t>(row);
                double nearest = INFINITY, farthest = -INFINITY;
                for (uint32_t ny = py > 0 ? py - 1 : 0; ny <= std::min(py + 1, size.height - 1); ny++)
                    for (uint32_t nx = px > 0 ? px - 1 : 0; nx <= std::min(px + 1, size.width - 1); nx++)
                    {
                        const float m = meters[size_t(ny) * size.width + nx];
                        if (std::isfinite(m))
                        {
                            nearest = std::min<double>(nearest, m);
                            farthest = std::max<double>(farthest, m);
                        }
                    }
                counts.tested++;
                if (w < nearest - std::max(0.05, 0.01 * nearest))
                    counts.in_front++;
                else if (w > farthest + std::max(0.05, 0.01 * farthest))
                    counts.behind++;
                else if (nearest <= farthest)
                    counts.matched++;
                else
                    counts.in_front++;
            }
        }
        return check;
    }

    Judgement judge_face(uint32_t face, const std::vector<SceneDrawRecord> &records, bool gbuffer, bool overflowed, bool can_grow,
        const std::optional<SceneCamera> &camera, const std::optional<Vec3d> &reference)
    {
        if (!gbuffer)
            return { FaceVerdict::no_gbuffer, {} };
        std::array<uint32_t, static_cast<size_t>(DrawSkip::count)> skips {};
        for (const SceneDrawRecord &record : records)
            skips[static_cast<size_t>(record.skip)]++;
        if (skips[0] == 0)
        {
            const auto most = std::max_element(skips.begin() + 1, skips.end());
            return { FaceVerdict::nothing, records.empty() ? std::string("the frame drew nothing into the G-buffer")
                : std::to_string(*most) + " of " + std::to_string(records.size()) + " draws were skipped for " + kDrawSkipNames[most - skips.begin()] };
        }
        if (!camera)
            return { FaceVerdict::no_camera, "neither the game's camera constants nor a pushed camera came with the frame" };
        if (!camera_faces(*camera, face))
        {
            char why[96];
            std::snprintf(why, sizeof why, "forward (%.3f, %.3f, %.3f)", camera->basis[2][0], camera->basis[2][1], camera->basis[2][2]);
            return { FaceVerdict::wrong_face, why };
        }
        if (reference && distance(camera->position, *reference) > 0.001)
        {
            char why[96];
            std::snprintf(why, sizeof why, "it was %.3f m from the first saved face's", distance(camera->position, *reference));
            return { FaceVerdict::moved, why };
        }
        if (overflowed && can_grow)
            return { FaceVerdict::overflow, {} };
        return { FaceVerdict::ok, {} };
    }

    SceneSchedule start_scene(const SceneRequest &request)
    {
        SceneSchedule s;
        s.request = request;
        if (request.faces >> kFaceView & 1)
            s.order.push_back(kFaceView);
        for (uint32_t face = 0; face < kFaceView; face++)
            if (request.faces >> face & 1)
                s.order.push_back(face);
        return s;
    }

    SceneTick scene_tick(SceneSchedule &s, const SceneFacts &facts)
    {
        using Phase = SceneSchedule::Phase;
        SceneTick tick;
        const auto stop = [&](std::string why) {
            s.phase = Phase::done;
            tick.stop = std::move(why);
            return tick;
        };
        if (s.phase == Phase::done)
        {
            tick.done = true;
            return tick;
        }
        if (facts.stop)
            return stop(*facts.stop);

        if (s.phase == Phase::read && facts.read)
        {
            const Judgement &j = *facts.read;
            const char *label = kFaces[s.face()].label;
            const uint32_t settle = s.face() == kFaceView ? kViewSettle : s.request.settle;
            switch (j.verdict)
            {
            case FaceVerdict::ok:
                s.saved++;
                s.next++;
                s.phase = Phase::settle;
                s.since = s.presents = s.attempts = s.reads = 0;
                if (s.next == s.order.size())
                {
                    s.phase = Phase::done;
                    tick.done = true;
                    return tick;
                }
                break;
            case FaceVerdict::not_ready:
                if (++s.reads < kSceneReads)
                    break;
                [[fallthrough]];
            case FaceVerdict::wrong_face:
            case FaceVerdict::no_gbuffer:
            case FaceVerdict::overflow:
            case FaceVerdict::unreadable:
            {
                const FaceVerdict verdict = j.verdict == FaceVerdict::not_ready ? FaceVerdict::unreadable : j.verdict;
                if (s.attempts >= kSceneAttempts)
                {
                    if (verdict == FaceVerdict::wrong_face)
                        return stop(std::string("the camera did not turn to ") + label + ": is CameraTools' free camera on?");
                    return stop(std::to_string(kSceneAttempts) + " captures of " + label + " failed, the last because "
                        + kFaceVerdictNames[static_cast<size_t>(verdict)] + (j.why.empty() ? "" : ": " + j.why));
                }
                // The camera has settled already, so the frame after this present is captured again.
                s.phase = Phase::settle;
                s.since = settle;
                s.reads = 0;
                break;
            }
            case FaceVerdict::moved:
            case FaceVerdict::nothing:
            case FaceVerdict::no_camera:
            case FaceVerdict::count:
                return stop(std::string(kFaceVerdictNames[static_cast<size_t>(j.verdict)]) + " at " + label + (j.why.empty() ? "" : ": " + j.why));
            }
        }

        const uint32_t face = s.face();
        if (s.presents >= kSceneFacePresents)
            return stop(std::string(kFaces[face].label) + " was not saved within " + std::to_string(kSceneFacePresents) + " presents");
        tick.publish_face = face;
        switch (s.phase)
        {
        case Phase::settle:
            if (s.since >= (face == kFaceView ? kViewSettle : s.request.settle) && facts.backlog < kSceneBacklog)
            {
                tick.capture_next_frame = true;
                s.attempts++;
                s.phase = Phase::copy;
                s.since = 0;
            }
            break;
        case Phase::copy:
            tick.copy_at_present = true;
            s.phase = Phase::read;
            s.since = 0;
            break;
        case Phase::read:
        case Phase::done:
            break;
        }
        s.since++;
        s.presents++;
        return tick;
    }
}
