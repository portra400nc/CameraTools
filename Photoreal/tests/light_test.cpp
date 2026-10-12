// The traced scene's light (core/light.h): the light word, the sRGB and R11G11B10 decodes, the albedo bake on a room
// seen from seven faces, the sky map, the ambient fit of build 208's meadow (fixtures/trace-180456-ambient.bin), the sun
// from the combine constants of captures 111824 and 214859 (fixtures/<capture>-combine-b0.bin and -b3.bin, which
// make_light_fixtures.py wrote) and trace-light.bin. With an output folder it also writes, with the add-on's own
// writers, a synthetic PHOTOREAL_SCENE_LIGHT | GBUFFER capture of the room without its ceiling, under a sky, and a save
// of its view, which run.sh hands to check_light.sh.
#include "../core/light.h"
#include "../core/scene_files.h"
#include "../core/trace_files.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

using namespace photoreal;

namespace
{
    int failures = 0;

    void check(const std::string &name, bool ok)
    {
        std::printf("%s %s\n", ok ? "PASS" : "FAIL", name.c_str());
        failures += ok ? 0 : 1;
    }

    bool near(float a, float b, float tolerance) { return std::fabs(a - b) <= tolerance; }
    bool near(const Vec3f &a, const Vec3f &b, float tolerance) { return near(a[0], b[0], tolerance) && near(a[1], b[1], tolerance) && near(a[2], b[2], tolerance); }

    std::string slurp(const std::string &path)
    {
        std::ifstream file(path, std::ios::binary);
        std::string bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        if (bytes.empty())
            throw std::runtime_error("missing " + path);
        return bytes;
    }

    void set_unity(float out[16], const double rows[16])
    {
        for (int r = 0; r < 4; r++)
            for (int c = 0; c < 4; c++)
                out[c * 4 + r] = float(rows[r * 4 + c]);
    }

    Vec3d cross(const Vec3d &a, const Vec3d &b) { return { a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0] }; }
    double dot(const Vec3d &a, const Vec3d &b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

    constexpr double kNear = 0.25, kFar = 1000;

    // A face's camera at position, as CameraTools pushes it: Unity's view and the reversed GPU projection, 90 degrees
    // vertical at aspect, unflipped.
    SceneCamera face_camera(const Vec3d &position, Vec3d forward, Vec3d up, double aspect, Camera *out = nullptr)
    {
        const Vec3d right = cross(up, forward);
        const double view[16] = { right[0], right[1], right[2], -dot(right, position), up[0], up[1], up[2], -dot(up, position), -forward[0], -forward[1],
            -forward[2], dot(forward, position), 0, 0, 0, 1 };
        const double projection[16] = { 1 / aspect, 0, 0, 0, 0, 1, 0, 0, 0, 0, kNear / (kFar - kNear), kFar * kNear / (kFar - kNear), 0, 0, -1, 0 };
        Camera pushed {};
        set_unity(pushed.world_to_view, view);
        set_unity(pushed.view_to_clip, projection);
        if (out)
            *out = pushed;
        return *scene_camera(pushed, false);
    }

    // The seven faces' directions: the cube's, then the view 30 degrees left of -Z and 10 down.
    std::array<std::pair<Vec3d, Vec3d>, kFaceCount> face_directions()
    {
        std::array<std::pair<Vec3d, Vec3d>, kFaceCount> out;
        for (uint32_t f = 0; f < 6; f++)
            out[f] = { kFaces[f].forward, kFaces[f].up };
        const double yaw = -150 * M_PI / 180, pitch = -10 * M_PI / 180;
        out[kFaceView] = { { std::cos(pitch) * std::sin(yaw), std::sin(pitch), std::cos(pitch) * std::cos(yaw) },
            { -std::sin(pitch) * std::sin(yaw), std::cos(pitch), -std::sin(pitch) * std::cos(yaw) } };
        return out;
    }

    // The room: a box 12 x 7 x 12 m around the camera, each side its own material and color in small triangles, a crate
    // in it, and behind the crate a plate of a material of its own that no face sees; and in the faces' images but not in
    // the traced scene two cards 1 m above the floor, one grass (stencil 129), one world (128).
    namespace room
    {
        constexpr uint32_t kWidth = 96, kHeight = 60;
        const Vec3d kReference = { 100.5, 20, -40.25 };

        struct Surface
        {
            std::vector<TraceTriangle> triangles;
            uint32_t material;
            std::array<uint8_t, 3> srgb;
            uint8_t stencil = 128;
        };

        void quad(Surface &s, Vec3f a, Vec3f b, Vec3f c, Vec3f d, int n)
        {
            const auto at = [&](float u, float v) {
                Vec3f p;
                for (int k = 0; k < 3; k++)
                    p[k] = a[k] + (b[k] - a[k]) * u + (d[k] - a[k]) * v;
                return p;
            };
            (void)c;
            for (int i = 0; i < n; i++)
                for (int j = 0; j < n; j++)
                {
                    TraceTriangle t1, t2;
                    t1.corner = { at(float(i) / n, float(j) / n), at(float(i + 1) / n, float(j) / n), at(float(i + 1) / n, float(j + 1) / n) };
                    t2.corner = { at(float(i) / n, float(j) / n), at(float(i + 1) / n, float(j + 1) / n), at(float(i) / n, float(j + 1) / n) };
                    s.triangles.push_back(t1);
                    s.triangles.push_back(t2);
                }
        }

        std::vector<Surface> surfaces(bool ceiling = true)
        {
            std::vector<Surface> out;
            // Without its ceiling, its walls end 1.5 m above the camera, so the sky fills the faces above.
            const float x0 = -6, x1 = 6, y0 = -3, y1 = ceiling ? 4 : 1.5f, z0 = -6, z1 = 6;
            const std::array<std::array<uint8_t, 3>, 6> colors = { { { 200, 60, 60 }, { 60, 200, 60 }, { 60, 60, 200 }, { 220, 220, 120 }, { 120, 220, 220 },
                { 230, 230, 230 } } };
            for (uint32_t side = 0; side < 6; side++)
            {
                Surface s;
                s.material = side;
                s.srgb = colors[side];
                switch (side)
                {
                case 0: quad(s, { x0, y0, z0 }, { x0, y0, z1 }, {}, { x0, y1, z0 }, 24); break;          // -X wall
                case 1: quad(s, { x1, y0, z0 }, { x1, y0, z1 }, {}, { x1, y1, z0 }, 24); break;          // +X wall
                case 2: quad(s, { x0, y0, z0 }, { x1, y0, z0 }, {}, { x0, y0, z1 }, 24); break;          // floor
                case 3: if (ceiling) quad(s, { x0, y1, z0 }, { x1, y1, z0 }, {}, { x0, y1, z1 }, 24); break; // ceiling
                case 4: quad(s, { x0, y0, z0 }, { x1, y0, z0 }, {}, { x0, y1, z0 }, 24); break;          // -Z wall
                case 5: quad(s, { x0, y0, z1 }, { x1, y0, z1 }, {}, { x0, y1, z1 }, 24); break;          // +Z wall
                }
                out.push_back(s);
            }
            Surface crate;
            crate.material = 6;
            crate.srgb = { 140, 90, 40 };
            const float cx0 = 2, cx1 = 3.5f, cy0 = -3, cy1 = 0, cz0 = -1, cz1 = 1;
            quad(crate, { cx0, cy0, cz0 }, { cx0, cy0, cz1 }, {}, { cx0, cy1, cz0 }, 2);
            quad(crate, { cx1, cy0, cz0 }, { cx1, cy0, cz1 }, {}, { cx1, cy1, cz0 }, 2);
            quad(crate, { cx0, cy1, cz0 }, { cx1, cy1, cz0 }, {}, { cx0, cy1, cz1 }, 2);
            quad(crate, { cx0, cy0, cz0 }, { cx1, cy0, cz0 }, {}, { cx0, cy1, cz0 }, 2);
            quad(crate, { cx0, cy0, cz1 }, { cx1, cy0, cz1 }, {}, { cx0, cy1, cz1 }, 2);
            out.push_back(crate);
            // Behind the crate, seen by no face: its material has no seen triangle.
            Surface plate;
            plate.material = 7;
            plate.srgb = { 255, 0, 255 };
            quad(plate, { 4.5f, -2.5f, -0.3f }, { 4.5f, -2.5f, 0.3f }, {}, { 4.5f, -1.5f, -0.3f }, 1);
            out.push_back(plate);
            // In the images only: the cards 1 m above the floor.
            Surface grass;
            grass.material = 100;
            grass.srgb = { 40, 160, 40 };
            grass.stencil = 129;
            quad(grass, { 1.5f, -2, -4.5f }, { 4.5f, -2, -4.5f }, {}, { 1.5f, -2, -1.5f }, 1);
            out.push_back(grass);
            Surface board = grass;
            board.material = 101;
            board.srgb = { 90, 90, 90 };
            board.stencil = 128;
            board.triangles.clear();
            quad(board, { 1.5f, -2, 1.5f }, { 4.5f, -2, 1.5f }, {}, { 1.5f, -2, 4.5f }, 1);
            out.push_back(board);
            return out;
        }

        // Each face's depth, albedo and stencil, ray cast through every surface, cards included; and its HDR scene: a sky
        // brighter toward the sun above, each surface its albedo times 0.8. normals, when asked for, gets the view face's
        // normals as the G-buffer stores them.
        std::vector<FaceImages> images(const std::vector<Surface> &all, std::vector<uint32_t> *normals = nullptr)
        {
            std::vector<TraceTriangle> triangles;
            std::vector<const Surface *> owner;
            for (const Surface &s : all)
                for (const TraceTriangle &t : s.triangles)
                {
                    triangles.push_back(t);
                    owner.push_back(&s);
                }
            const Bvh bvh = build_bvh(triangles);
            std::vector<FaceImages> out;
            const auto directions = face_directions();
            for (uint32_t face = 0; face < kFaceCount; face++)
            {
                const SceneCamera camera = face_camera(kReference, directions[face].first, directions[face].second, double(kWidth) / kHeight);
                FaceImages f = *face_images(face, camera, { kWidth, kHeight });
                f.depth.assign(size_t(kWidth) * kHeight, 0);
                f.albedo.assign(size_t(kWidth) * kHeight * 4, 0);
                f.stencil.assign(size_t(kWidth) * kHeight, 0);
                f.hdr.assign(size_t(kWidth) * kHeight, 0);
                if (normals && face == kFaceView)
                    normals->assign(size_t(kWidth) * kHeight, 0);
                for (uint32_t py = 0; py < kHeight; py++)
                    for (uint32_t px = 0; px < kWidth; px++)
                    {
                        const float ndc_x = (px + 0.5f) / kWidth * 2 - 1, ndc_y = 1 - (py + 0.5f) / kHeight * 2;
                        const Vec3d at = *clip_to_world(camera, { ndc_x, ndc_y, 1, 1 });
                        Vec3d d = { at[0] - kReference[0], at[1] - kReference[1], at[2] - kReference[2] };
                        const double length = std::sqrt(dot(d, d));
                        d = { d[0] / length, d[1] / length, d[2] / length };
                        const std::optional<Hit> hit = trace_nearest(bvh, { 0, 0, 0 }, { float(d[0]), float(d[1]), float(d[2]) }, 1e4f);
                        if (!hit)
                        {
                            const float bright = float(0.6 + 0.8 * std::max(0.0, d[0] * 0.5 + d[1] * 0.8));
                            f.hdr[size_t(py) * kWidth + px] = hdr_stored({ 0.5f * bright, 0.7f * bright, bright });
                            continue;
                        }
                        const double distance = hit->t * dot(d, camera.basis[2]);
                        const size_t i = size_t(py) * kWidth + px;
                        const Surface &s = *owner[bvh.source[hit->triangle]];
                        f.depth[i] = float(kNear * (kFar - distance) / ((kFar - kNear) * distance));
                        f.stencil[i] = s.stencil;
                        for (int k = 0; k < 3; k++)
                            f.albedo[i * 4 + k] = s.srgb[k];
                        f.albedo[i * 4 + 3] = 255;
                        f.hdr[i] = hdr_stored({ 0.8f * srgb_linear(s.srgb[0]), 0.8f * srgb_linear(s.srgb[1]), 0.8f * srgb_linear(s.srgb[2]) });
                        if (normals && face == kFaceView)
                        {
                            const BvhTriangle &t = bvh.triangles[hit->triangle];
                            Vec3d n = cross({ t.e1[0], t.e1[1], t.e1[2] }, { t.e2[0], t.e2[1], t.e2[2] });
                            const double l = std::sqrt(dot(n, n)) * (dot(n, d) > 0 ? -1 : 1);
                            uint32_t word = 0;
                            for (int k = 0; k < 3; k++)
                                word |= uint32_t(std::lround((n[k] / l * 0.5 + 0.5) * 1023)) << (k * 10);
                            (*normals)[i] = word;
                        }
                    }
                out.push_back(std::move(f));
            }
            return out;
        }
    }

    // Six cube faces of sky at the origin: color everywhere, or only above the horizon with geometry below.
    std::vector<FaceImages> sky_faces(const Vec3f &color, bool ground, uint32_t width, uint32_t height)
    {
        std::vector<FaceImages> out;
        for (uint32_t face = 0; face < 6; face++)
        {
            const SceneCamera camera = face_camera({ 0, 0, 0 }, kFaces[face].forward, kFaces[face].up, double(width) / height);
            FaceImages f = *face_images(face, camera, { width, height });
            f.depth.assign(size_t(width) * height, 0);
            f.hdr.assign(size_t(width) * height, hdr_stored(color));
            for (uint32_t py = 0; py < height; py++)
                for (uint32_t px = 0; px < width; px++)
                {
                    const Vec3d at = *clip_to_world(camera, { (px + 0.5f) / width * 2 - 1, 1 - (py + 0.5f) / height * 2, 1, 1 });
                    if (ground && at[1] < 0)
                    {
                        f.depth[size_t(py) * width + px] = 0.01f;
                        f.hdr[size_t(py) * width + px] = hdr_stored({ 0.02f, 0.02f, 0.02f });
                    }
                }
            out.push_back(std::move(f));
        }
        return out;
    }

    CombineConstants combine_of(const std::string &capture)
    {
        const std::string b0 = slurp("fixtures/" + capture + "-combine-b0.bin"), b3 = slurp("fixtures/" + capture + "-combine-b3.bin");
        CombineConstants c;
        c.bytes = { static_cast<uint32_t>(b0.size()), 192, 352, static_cast<uint32_t>(b3.size()) };
        std::memcpy(c.b0.data(), b0.data(), 16);
        std::memcpy(c.b3.data(), b3.data(), 16);
        return c;
    }

    SunCbuffers sun_of(const std::string &capture)
    {
        SunCbuffers out {};
        size_t at = 0;
        for (int b = 0; b < 4; b++)
        {
            const std::string bytes = slurp("fixtures/" + capture + "-sun-b" + std::to_string(b) + ".bin");
            std::memcpy(out.data() + at, bytes.data(), bytes.size());
            at += bytes.size();
        }
        return out;
    }
}

namespace
{
    // The synthetic PHOTOREAL_SCENE_LIGHT | GBUFFER capture of the open room and a save of its view, by the add-on's writers.
    void write_synthetic(const std::filesystem::path &out)
    {
        const std::vector<room::Surface> surfaces = room::surfaces(false);
        std::vector<uint32_t> view_normals;
        std::vector<FaceImages> faces = room::images(surfaces, &view_normals);
        std::vector<TraceTriangle> triangles;
        std::vector<uint32_t> materials;
        for (size_t s = 0; s + 2 < surfaces.size(); s++)
            for (const TraceTriangle &t : surfaces[s].triangles)
            {
                triangles.push_back(t);
                materials.push_back(surfaces[s].material);
            }
        SceneFolder folder;
        folder.path = out / "scene-light-synthetic";
        begin_scene_folder(folder);
        SceneRequest request;
        request.trace = request.light = request.gbuffer = request.no_meshes = true;
        LightInputs inputs;
        const auto directions = face_directions();
        Camera view_camera {};
        for (FaceImages &f : faces)
        {
            Camera pushed {};
            SceneFaceData data;
            data.face = f.face;
            data.frame = 100 + f.face;
            data.attempts = 1;
            data.camera = face_camera(room::kReference, directions[f.face].first, directions[f.face].second, double(room::kWidth) / room::kHeight, &pushed);
            data.pushed = pushed;
            data.status = "96x60 synthetic";
            data.render = f.size;
            data.origin = room::kReference;
            data.depth = f.depth;
            data.stencil = f.stencil;
            data.albedo = f.albedo;
            data.hdr = f.hdr;
            if (f.face == kFaceView)
            {
                view_camera = pushed;
                // The game's irradiance a function of the normal alone, as the meadow's.
                data.normals.resize(view_normals.size() * 4);
                data.irradiance.resize(view_normals.size() * 4);
                for (size_t i = 0; i < view_normals.size(); i++)
                {
                    const float ny = (view_normals[i] >> 10 & 1023u) / 1023.0f * 2 - 1;
                    const uint32_t irradiance = hdr_stored({ 0.2f + 0.13f * ny, 0.43f + 0.16f * ny, 0.61f + 0.2f * ny });
                    std::memcpy(&data.normals[i * 4], &view_normals[i], 4);
                    std::memcpy(&data.irradiance[i * 4], &irradiance, 4);
                }
                inputs.first_size = f.size;
                inputs.first_normals = data.normals;
                inputs.first_irradiance = data.irradiance;
                inputs.first_stencil = data.stencil;
                inputs.first_depth = data.depth;
            }
            write_scene_face(folder, data, request);
        }
        inputs.faces = faces;
        inputs.combine = CombineConstants { kCombineCbufferBytes, { 2.8f, 2.5f, 1.13f, 1.6f }, { 0.259f, 0.5247f, -0.811f, 0 } };
        inputs.cascade = Vec3 { 0.2491f, 0.5736f, -0.7803f };
        inputs.pushed_sky = { 0.3f, 0.5f, 0.8f };
        std::vector<TraceTriangle> baked = triangles;
        const SceneLight light = build_light(baked, materials, inputs, room::kReference, 3);
        const std::string scene_bytes = trace_scene_bytes(triangles, room::kReference);
        std::ofstream(folder.path / "trace-scene.bin", std::ios::binary).write(scene_bytes.data(), static_cast<std::streamsize>(scene_bytes.size()));
        const std::string light_file = light_bytes(baked, materials, light, faces);
        std::ofstream(folder.path / "trace-light.bin", std::ios::binary).write(light_file.data(), static_cast<std::streamsize>(light_file.size()));

        // The view's save: its G-buffer as the trace pass found it, and the frame the combine pass lit, sun and all.
        const FaceImages &view = faces.back();
        TraceSave save;
        save.size = view.size;
        TraceSettings settings;
        save.constants = *trace_constants(settings, 1, view_camera, false, view.size, room::kReference, *inputs.cascade, 1);
        save.history.assign(size_t(view.size.width) * view.size.height * 4, 0);
        save.depth = view.depth;
        save.stencil = view.stencil;
        save.normals = view_normals;
        save.albedo = view.albedo;
        save.camera = view_camera;
        save.reference = room::kReference;
        save.scene = folder.path.filename().u8string();
        save.irradiance = inputs.first_irradiance;
        save.mask.assign(save.depth.size() * 2, 255);
        save.hdr.resize(save.depth.size() * 4);
        for (size_t i = 0; i < save.depth.size(); i++)
        {
            const Vec3f irradiance = hdr_rgb(*reinterpret_cast<const uint32_t *>(&save.irradiance[i * 4]));
            Vec3f n;
            for (int k = 0; k < 3; k++)
                n[k] = (view_normals[i] >> (k * 10) & 1023u) / 1023.0f * 2 - 1;
            const float cosine = std::max(0.0f, n[0] * light.sun.shade[0] + n[1] * light.sun.shade[1] + n[2] * light.sun.shade[2]);
            Vec3f lit;
            for (int k = 0; k < 3; k++)
                lit[k] = srgb_linear(view.albedo[i * 4 + k]) * (light.sun.color[k] * cosine + irradiance[k]);
            const uint32_t stored = view.depth[i] > 0 ? hdr_stored(lit) : view.hdr[i];
            std::memcpy(&save.hdr[i * 4], &stored, 4);
        }
        write_trace_folder(out / "trace-light-synthetic", save);
    }
}

int main(int argc, char **argv)
{
    {
        const uint32_t word = pack_light({ 0.25f, 0.5f, 1.0f }, AlbedoSource::covered);
        const Vec3f back = light_albedo(word);
        check("the light word holds the albedo's square roots in 10 bits and the source in 2: 0.25, 0.5 and 1 round trip within 1/1023 of their roots",
            word == (512u | 723u << 10 | 1023u << 20 | 1u << 30) && light_source(word) == AlbedoSource::covered
            && near(std::sqrt(back[0]), 0.5f, 0.5f / 1023) && near(std::sqrt(back[1]), std::sqrt(0.5f), 0.5f / 1023) && back[2] == 1.0f);
        const Vec3 kernel = kernel_albedo(word);
        check("the kernel's albedo_of reads the word as light_albedo does", kernel[0] == back[0] && kernel[1] == back[1] && kernel[2] == back[2]);
        check("an albedo outside 0..1 clamps", pack_light({ -1, 2, NAN }, AlbedoSource::direct) == (0u | 1023u << 10 | 0u << 20));
        check("sRGB bytes decode to linear: 0, 1, and 128 to 0.2158605", srgb_linear(0) == 0 && srgb_linear(255) == 1.0f
            && near(srgb_linear(128), 0.2158605f, 1e-6f) && near(srgb_linear(10), 10 / 255.0f / 12.92f, 1e-7f));
        bool round_trip = true;
        for (uint32_t bits = 0; bits < 31u * 64; bits++)
            round_trip &= hdr_stored(hdr_rgb(bits | bits << 11 | (bits >> 1) << 22)) == (bits | bits << 11 | (bits >> 1) << 22);
        check("every finite R11G11B10_FLOAT value decodes and encodes back to itself", round_trip);
        check("R11G11B10_FLOAT decodes 1, 0.5 and 2^-15", hdr_rgb(15u << 6 | (14u << 6) << 11 | (15u << 5) << 22) == Vec3f { 1, 0.5f, 1 }
            && hdr_rgb(32) == Vec3f { std::ldexp(1.0f, -15), 0, 0 });
    }
    {
        const std::vector<room::Surface> surfaces = room::surfaces();
        const std::vector<FaceImages> faces = room::images(surfaces);
        std::vector<TraceTriangle> triangles;
        std::vector<uint32_t> materials;
        // The traced scene leaves the two cards out. Of each surface, the triangle whose centroid is nearest a point.
        std::vector<std::pair<size_t, Vec3f>> marks;
        for (size_t s = 0; s + 2 < surfaces.size(); s++)
            for (const TraceTriangle &t : surfaces[s].triangles)
            {
                triangles.push_back(t);
                materials.push_back(surfaces[s].material);
            }
        const auto nearest = [&](const Vec3f &p) {
            size_t best = 0;
            float best_d = INFINITY;
            for (size_t i = 0; i < triangles.size(); i++)
            {
                float d = 0;
                for (int k = 0; k < 3; k++)
                {
                    const float c = (triangles[i].corner[0][k] + triangles[i].corner[1][k] + triangles[i].corner[2][k]) / 3 - p[k];
                    d += c * c;
                }
                if (d < best_d)
                {
                    best_d = d;
                    best = i;
                }
            }
            return best;
        };
        const size_t ceiling = nearest({ 0.1f, 4, 0.1f }), hidden_wall = nearest({ 6, -1.5f, 0.05f }), under_grass = nearest({ 3, -3, -3 }),
            under_board = nearest({ 3, -3, 3 }), plate = nearest({ 4.5f, -2, 0 });
        std::vector<TraceTriangle> one = triangles;
        const AlbedoCounts counts = bake_albedo(triangles, materials, faces, room::kReference, 4);
        bake_albedo(one, materials, faces, room::kReference, 1);
        bool same = true;
        for (size_t i = 0; i < triangles.size(); i++)
            same &= triangles[i].light == one[i].light;
        std::printf("INFO the room's %zu triangles bake %llu direct, %llu covered, %llu by material and %llu by class; areas %.2f, %.2f, %.2f and %.2f m2\n",
            triangles.size(), static_cast<unsigned long long>(counts.triangles[0]), static_cast<unsigned long long>(counts.triangles[1]),
            static_cast<unsigned long long>(counts.triangles[2]), static_cast<unsigned long long>(counts.triangles[3]), counts.area[0], counts.area[1], counts.area[2],
            counts.area[3]);
        check("the room bakes the same light words on 1 thread and on 4", same);
        const auto color = [](const std::array<uint8_t, 3> &srgb) { return Vec3f { srgb_linear(srgb[0]), srgb_linear(srgb[1]), srgb_linear(srgb[2]) }; };
        check("a seen ceiling triangle takes the ceiling's color, to the word", triangles[ceiling].light == pack_light(color(surfaces[3].srgb), AlbedoSource::direct));
        // A mean of equal colors rounds within one step of the word.
        const auto close = [](uint32_t a, uint32_t b) {
            bool ok = true;
            for (int k = 0; k < 3; k++)
                ok &= std::abs(int(a >> (k * 10) & 1023u) - int(b >> (k * 10) & 1023u)) <= 1;
            return ok;
        };
        check("a wall triangle behind the crate takes its material's mean, the wall's color",
            light_source(triangles[hidden_wall].light) == AlbedoSource::material && close(triangles[hidden_wall].light, pack_light(color(surfaces[1].srgb), AlbedoSource::direct)));
        check("a floor triangle under the grass card 1 m above it takes the grass's color, covered",
            triangles[under_grass].light == pack_light(color(surfaces[8].srgb), AlbedoSource::covered));
        // The floor's mean takes in its triangles under the grass, covered.
        const Vec3f board = light_albedo(triangles[under_board].light), floor = color(surfaces[2].srgb), grass = color(surfaces[8].srgb);
        check("a floor triangle under the stencil-128 board takes the floor's mean, its blue with a little of the grass's green, not the board's gray",
            light_source(triangles[under_board].light) == AlbedoSource::material && board[2] < floor[2] && board[2] > 0.9f * floor[2] && board[1] > floor[1]
                && board[1] < 0.1f * grass[1] + floor[1]);
        const Vec3f plate_albedo = light_albedo(triangles[plate].light);
        check("the plate no face sees, of a material none sees, takes the world class's mean, neither magenta nor black",
            light_source(triangles[plate].light) == AlbedoSource::class_mean && plate_albedo[1] > 0.1f && plate_albedo[0] < 0.9f);
        check("the room's sources by triangle: 6309 direct, 98 covered, 545 by material and 2 by class",
            counts.triangles == std::array<uint64_t, 4> { 6309, 98, 545, 2 });
        const AlbedoCounts recount = count_albedo(triangles);
        check("count_albedo counts the baked words as the bake did", recount.triangles == counts.triangles && near(float(recount.area[2]), float(counts.area[2]), 1e-3f));
        std::vector<TraceTriangle> no_faces = triangles;
        const AlbedoCounts none = bake_albedo(no_faces, materials, {}, room::kReference, 2);
        check("without faces every triangle takes kNoAlbedo, by class", none.triangles[3] == triangles.size()
            && light_albedo(no_faces[0].light)[0] == light_albedo(pack_light({ kNoAlbedo, kNoAlbedo, kNoAlbedo }, AlbedoSource::class_mean))[0]);
        FaceImages half = faces[0];
        halve_face(half);
        check("a face kept at half size has half the pixels, its depth the nearest of each four",
            half.size == Size { 48, 30 } && half.depth.size() == 48 * 30 && half.albedo.size() == 48 * 30 * 4 && half.depth[0] >= faces[0].depth[0]
            && half.depth[0] >= faces[0].depth[1] && half.depth[0] >= faces[0].depth[96]);
    }
    {
        const Vec3f blue = { 0.5f, 0.25f, 1.0f };
        const SkyMap sky = build_sky_map(sky_faces(blue, false, 128, 80), { 9, 9, 9 });
        bool all = true;
        for (size_t i = 0; i < sky.texels.size(); i += 4)
            all &= sky.texels[i] == blue[0] && sky.texels[i + 1] == blue[1] && sky.texels[i + 2] == blue[2] && sky.texels[i + 3] == 1;
        check("six faces of one sky color give that color in every texel, all real", all && sky.real == kSkyMapSide * kSkyMapSide && !sky.fallback);
        bool even = true;
        for (const Vec3f &n : { Vec3f { 1, 0, 0 }, Vec3f { 0, 1, 0 }, Vec3f { 0, 0, -1 }, Vec3f { 0.6f, -0.8f, 0 } })
            even &= near(sky_irradiance(sky, n), blue, 2e-3f);
        check("an even sky's harmonics give its color on every surface", even);

        const SkyMap half = build_sky_map(sky_faces(blue, true, 128, 80), { 9, 9, 9 });
        uint32_t filled = 0;
        bool color = true;
        for (size_t i = 0; i < half.texels.size(); i += 4)
        {
            filled += half.texels[i + 3] == 0;
            color &= half.texels[i] == blue[0] && half.texels[i + 1] == blue[1] && half.texels[i + 2] == blue[2];
        }
        std::printf("INFO a sky above ground: %u real texels, %u filled\n", half.real, filled);
        check("faces with ground below the horizon fill the lower half's texels from the sky above", color && filled > 6000 && half.real > 6000
            && half.real + filled == kSkyMapSide * kSkyMapSide);
        const Vec3f down = sky_irradiance(half, { 0, -1, 0 }), up = sky_irradiance(half, { 0, 1, 0 });
        check("the filled sky lights a surface facing down as one facing up", near(down, blue, 3e-3f) && near(up, blue, 3e-3f));

        std::vector<FaceImages> bright = sky_faces(blue, false, 128, 80);
        for (uint32_t y = 36; y < 44; y++)
            for (uint32_t x = 60; x < 68; x++)
                bright[2].hdr[y * 128 + x] = hdr_stored({ 50, 25, 100 });
        const SkyMap capped = build_sky_map(bright, { 9, 9, 9 });
        double brightest = 0;
        const double median = 0.2126 * blue[0] + 0.7152 * blue[1] + 0.0722 * blue[2];
        for (size_t i = 0; i < capped.texels.size(); i += 4)
            brightest = std::max(brightest, 0.2126 * capped.texels[i] + 0.7152 * capped.texels[i + 1] + 0.0722 * capped.texels[i + 2]);
        check("a sun 100 times the sky's median is capped at 8 times it", near(float(brightest), float(8 * median), 1e-4f));

        std::vector<FaceImages> dark = sky_faces(blue, false, 128, 80);
        for (FaceImages &f : dark)
            f.hdr.clear();
        const SkyMap fallback = build_sky_map(dark, { 0.3f, 0.4f, 0.5f });
        check("faces without HDR copies give the pushed sky color in every texel, filled", fallback.fallback && fallback.real == 0
            && fallback.texels[0] == 0.3f && fallback.texels[kSkyMapSide * kSkyMapSide * 4 - 2] == 0.5f && fallback.texels[3] == 0
            && near(sky_irradiance(fallback, { 0, 1, 0 }), { 0.3f, 0.4f, 0.5f }, 2e-3f));
        bool lands = true;
        for (uint32_t i = 0; i < kSkyMapSide * kSkyMapSide; i += 7)
        {
            const uint32_t x = i % kSkyMapSide, y = i / kSkyMapSide;
            lands &= sky_texel(sky_direction((x + 0.5f) / kSkyMapSide, (y + 0.5f) / kSkyMapSide)) == i;
        }
        check("the kernel's sky_texel_of reads every 7th texel back from the direction the build filled it from", lands);
        const Vec3f straight = sky_direction(0.5f, 0.5f), under = sky_direction(0.001f, 0.001f);
        check("the map's center looks up and its corners down", near(straight, { 0, 1, 0 }, 1e-6f) && under[1] < -0.99f);
    }
    {
        const std::string bytes = slurp("fixtures/trace-180456-ambient.bin");
        uint32_t count = 0;
        std::memcpy(&count, bytes.data() + 4, 4);
        std::vector<uint8_t> normals(size_t(count) * 4), irradiance(size_t(count) * 4), stencil(count, 128);
        std::vector<float> depth(count, 0.01f);
        for (uint32_t i = 0; i < count; i++)
        {
            std::memcpy(&normals[i * 4], bytes.data() + 8 + i * 8, 4);
            std::memcpy(&irradiance[i * 4], bytes.data() + 12 + i * 8, 4);
        }
        const AmbientFit fit = fit_ambient({ count, 1 }, irradiance, normals, stencil, depth, { 1, 1, 1 });
        std::printf("INFO the meadow's fit: (%.6f %.6f %.6f) (%.6f %.6f %.6f) (%.6f %.6f %.6f) (%.6f %.6f %.6f), r2 %.5f %.5f %.5f\n", fit.rows[0][0], fit.rows[0][1],
            fit.rows[0][2], fit.rows[1][0], fit.rows[1][1], fit.rows[1][2], fit.rows[2][0], fit.rows[2][1], fit.rows[2][2], fit.rows[3][0], fit.rows[3][1],
            fit.rows[3][2], fit.r2[0], fit.r2[1], fit.r2[2]);
        check("build 208's meadow: the fit of its irradiance to its normals is numpy's, (0.193353, 0.428513, 0.610895) + n.y (0.136876, 0.16, 0.204164) and "
              "small x and z rows",
            fit.kind == AmbientFit::Kind::fit && fit.pixels == count && count == 49761 && near(fit.rows[0], { 0.193353f, 0.428513f, 0.610895f }, 2e-6f)
                && near(fit.rows[1], { 0.000101f, -0.000753f, -0.000445f }, 2e-6f) && near(fit.rows[2], { 0.136876f, 0.160000f, 0.204164f }, 2e-6f)
                && near(fit.rows[3], { -0.000594f, 0.003477f, 0.000728f }, 2e-6f) && near(fit.r2, { 0.99949f, 0.99699f, 0.99844f }, 2e-5f));
        const AmbientFit few = fit_ambient({ 100, 1 }, std::vector<uint8_t>(irradiance.begin(), irradiance.begin() + 400),
            std::vector<uint8_t>(normals.begin(), normals.begin() + 400), std::vector<uint8_t>(100, 128), std::vector<float>(100, 0.01f), { 1, 1, 1 });
        check("100 pixels give their mean", few.kind == AmbientFit::Kind::mean && near(few.rows[0], { 0.329023f, 0.585938f, 0.8125f }, 1e-6f)
            && few.rows[2] == Vec3f {});
        const AmbientFit none = fit_ambient({ 100, 1 }, {}, {}, {}, {}, { 0.2f, 0.3f, 0.4f });
        check("without the copies, the pushed sky color", none.kind == AmbientFit::Kind::pushed && none.rows[0] == Vec3f { 0.2f, 0.3f, 0.4f });
        // Up-facing pixels whose light grows from 0.05 to 0.45 with n.y: a fit whose slope passes its constant, scaled back
        // so no normal gets a negative light.
        std::vector<uint8_t> steep_n(20000 * 4), steep_i(20000 * 4);
        for (uint32_t i = 0; i < 20000; i++)
        {
            const float y = (i + 0.5f) / 20000;
            const float horizontal = std::sqrt(std::max(0.0f, 1 - y * y));
            const float turn = 0.37f * i;
            const uint32_t n = uint32_t(std::lround((horizontal * std::cos(turn) * 0.5f + 0.5f) * 1023)) | uint32_t(std::lround((y * 0.5f + 0.5f) * 1023)) << 10
                | uint32_t(std::lround((horizontal * std::sin(turn) * 0.5f + 0.5f) * 1023)) << 20;
            const uint32_t value = hdr_stored({ 0.05f + 0.4f * y, 0.2f, 0.2f });
            std::memcpy(&steep_n[i * 4], &n, 4);
            std::memcpy(&steep_i[i * 4], &value, 4);
        }
        const AmbientFit steep = fit_ambient({ 20000, 1 }, steep_i, steep_n, std::vector<uint8_t>(20000, 128), std::vector<float>(20000, 0.01f), {});
        const double slope = std::sqrt(double(steep.rows[1][0]) * steep.rows[1][0] + double(steep.rows[2][0]) * steep.rows[2][0] + double(steep.rows[3][0]) * steep.rows[3][0]);
        check("a fit steeper than its constant is scaled to light a surface facing away with 0", steep.kind == AmbientFit::Kind::fit
            && near(float(slope), steep.rows[0][0], 1e-6f));
    }
    {
        const SunFacts a_cascade = sun_facts(sun_of("capture-20261010-111824")), b_cascade = sun_facts(sun_of("capture-20261009-214859"));
        const SunLight a = sun_light(combine_of("capture-20261010-111824"), a_cascade.toward_sun, { 1, 1, 1 }, { 0, 1, 0 });
        const SunLight b = sun_light(combine_of("capture-20261009-214859"), b_cascade.toward_sun, { 1, 1, 1 }, { 0, 1, 0 });
        std::printf("INFO capture 111824: cascade 0 (%.6f, %.6f, %.6f), the shading direction (%.6f, %.6f, %.6f)\n", (*a_cascade.toward_sun)[0],
            (*a_cascade.toward_sun)[1], (*a_cascade.toward_sun)[2], a.shade[0], a.shade[1], a.shade[2]);
        check("capture 111824's combine draw gives the sun's color (2.804184, 2.501149, 1.130967) and shading direction (0.258915, 0.524667, -0.810979)",
            a.color_source == LightSource::game && a.shade_source == LightSource::game && a.why.empty() && near(a.color, { 2.804184f, 2.501149f, 1.130967f }, 1e-6f)
                && near(a.shade, { 0.258915f, 0.524667f, -0.810979f }, 1e-6f));
        check("capture 214859's gives (4.225881, 3.942333, 1.566667) and (0.231102, 0.370443, -0.899646), 13 degrees under cascade 0's 35",
            b.color_source == LightSource::game && b.shade_source == LightSource::game && near(b.color, { 4.225881f, 3.942333f, 1.566667f }, 1e-6f)
                && near(b.shade, { 0.231102f, 0.370443f, -0.899646f }, 1e-6f) && near((*b_cascade.toward_sun)[1], 0.573576f, 1e-6f));
        CombineConstants sized = combine_of("capture-20261010-111824");
        sized.bytes[0] = 2592;
        const SunLight other = sun_light(sized, a_cascade.toward_sun, { 1, 2, 3 }, { 0, 1, 0 });
        check("constant buffers of other sizes give the pushed color and direction", other.why == "sizes" && other.color == Vec3 { 1, 2, 3 }
            && other.shade == Vec3 { 0, 1, 0 } && other.color_source == LightSource::pushed && other.shade_source == LightSource::pushed);
        CombineConstants skewed = combine_of("capture-20261010-111824");
        const float c = std::cos(0.0873f), s = std::sin(0.0873f), x = skewed.b3[0], z = skewed.b3[2];
        skewed.b3[0] = c * x - s * z;
        skewed.b3[2] = s * x + c * z;
        const SunLight turned = sun_light(skewed, a_cascade.toward_sun, { 1, 2, 3 }, { 0, 1, 0 });
        check("a shading direction 5 degrees off cascade 0's azimuth is refused, the color kept", turned.why == "azimuth"
            && turned.shade_source == LightSource::pushed && turned.color_source == LightSource::game);
        CombineConstants nan = combine_of("capture-20261010-111824");
        nan.b0[1] = NAN;
        const SunLight broken = sun_light(nan, a_cascade.toward_sun, { 1, 2, 3 }, { 0, 1, 0 });
        check("a NaN in the color gives the pushed color and keeps the game's direction", broken.why == "color" && broken.color == Vec3 { 1, 2, 3 }
            && broken.shade_source == LightSource::game);
        check("no constants read: the pushed values", sun_light(std::nullopt, std::nullopt, { 1, 2, 3 }, { 0, 1, 0 }).why == "unread");
    }
    {
        std::vector<TraceTriangle> triangles(3);
        triangles[1].light = pack_light({ 0.5f, 0.25f, 0.125f }, AlbedoSource::direct);
        triangles[2].light = pack_light({ 0.1f, 0.2f, 0.3f }, AlbedoSource::material);
        SceneLight light;
        light.sky = build_sky_map(sky_faces({ 0.5f, 0.25f, 1 }, true, 32, 20), { 1, 1, 1 });
        light.ambient.rows = { { { 1, 2, 3 }, { 4, 5, 6 }, { 7, 8, 9 }, { 10, 11, 12 } } };
        light.ambient.kind = AmbientFit::Kind::fit;
        light.sun.color = { 2.8f, 2.5f, 1.1f };
        light.sun.shade = { 0.26f, 0.52f, -0.81f };
        light.sun.color_source = LightSource::game;
        light.cascade = { 0.25f, 0.57f, -0.78f };
        std::vector<FaceImages> faces = sky_faces({ 0.5f, 0.25f, 1 }, false, 32, 20);
        faces.resize(2);
        const std::string bytes = light_bytes(triangles, { 0, 5, 9 }, light, faces);
        const std::optional<LightFile> back = parse_light(bytes);
        check("trace-light.bin reads back its words, materials, sky map, harmonics, fit, sun and the faces' cameras",
            back && back->words == std::vector<uint32_t> { 0, triangles[1].light, triangles[2].light } && back->materials == std::vector<uint32_t> { 0, 5, 9 }
                && back->light.sky.texels == light.sky.texels && back->light.sky.real == light.sky.real && back->light.sky.harmonics == light.sky.harmonics
                && back->light.ambient.rows == light.ambient.rows && back->light.ambient.kind == AmbientFit::Kind::fit && back->light.sun.color == light.sun.color
                && back->light.sun.color_source == LightSource::game && back->light.sun.shade_source == LightSource::pushed && back->light.cascade == light.cascade
                && back->faces.size() == 2 && back->faces[1].face == 1 && back->faces[1].size == Size { 32, 20 }
                && back->faces[1].clip_to_world == faces[1].clip_to_world && back->faces[1].world_to_clip == faces[1].world_to_clip
                && bytes.size() == 8 + 3 * 8 + 4 + kSkyMapSide * kSkyMapSide * 16 + 8 + 9 * 12 + 4 * 12 + 12 + 8 + 3 * 12 + 8 + 4 + 2 * (12 + 128));
        check("trace-light.bin cut short or with a wrong magic reads as nothing", !parse_light(bytes.substr(0, bytes.size() - 1)) && !parse_light("PRTS" + bytes.substr(4)));
        light.albedo = count_albedo(triangles);
        check("the scene's light flags: albedo, sky, the ambient fit, not the sun with a pushed direction",
            light.flags() == (kLightAlbedo | kLightSky | kLightAmbient));
    }
    if (argc > 1)
    {
        write_synthetic(argv[1]);
        check("the synthetic light capture is written with its faces' raw copies, trace-scene.bin and trace-light.bin",
            std::filesystem::exists(std::filesystem::path(argv[1]) / "scene-light-synthetic" / "face-3-hdr.r11g11b10")
                && std::filesystem::file_size(std::filesystem::path(argv[1]) / "scene-light-synthetic" / "face-6-albedo.rgba8") == room::kWidth * room::kHeight * 4
                && std::filesystem::exists(std::filesystem::path(argv[1]) / "scene-light-synthetic" / "trace-light.bin")
                && std::filesystem::exists(std::filesystem::path(argv[1]) / "trace-light-synthetic" / "albedo.rgba8"));
    }
    std::printf("%s: %d failed\n", __FILE__, failures);
    return failures == 0 ? 0 : 1;
}
