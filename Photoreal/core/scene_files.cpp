#include "scene_files.h"
#include "png.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <unordered_map>

namespace photoreal
{
    namespace
    {
        namespace fs = std::filesystem;

        // A corner's clip-space bits, which identify the vertex shader invocation that wrote it.
        using Bits = std::array<uint32_t, 4>;

        struct BitsHash
        {
            size_t operator()(const Bits &b) const
            {
                uint64_t h = 0xcbf29ce484222325ull;
                for (uint32_t v : b)
                    h = (h ^ v) * 0x100000001b3ull;
                return static_cast<size_t>(h ^ (h >> 32));
            }
        };

        // Adding 0 turns -0 into 0, so a coordinate never prints as "-0".
        std::string format(const char *pattern, double value)
        {
            char text[48];
            std::snprintf(text, sizeof text, pattern, value + 0.0);
            return text;
        }

        std::string hex(uint64_t value)
        {
            if (value == 0)
                return "-";
            char text[24];
            std::snprintf(text, sizeof text, "%016llx", static_cast<unsigned long long>(value));
            return text;
        }

        std::string joined(const double *values, size_t count)
        {
            std::string out;
            for (size_t i = 0; i < count; i++)
                out += (i == 0 ? "" : ",") + format("%.9g", values[i]);
            return out;
        }

        std::string vec(const Vec3d &v) { return joined(v.data(), 3); }

        const char *const kDepthFuncNames[] = { "off", "never", "less", "equal", "less-equal", "greater", "not-equal", "greater-equal", "always" };

        // Both writers walk the draws the same way: whole draws until max_triangles, corners merged within a draw by their
        // clip bits, each placed once, and a triangle dropped when a corner is at infinity. vertex() gets each new
        // corner's mirrored position relative to the origin and returns its index; triangle() gets the indices in written
        // order, already swapped.
        template <class Begin, class Vertex, class Triangle>
        MeshStats walk(const SceneCamera &camera, const Vec3d &origin, const std::vector<SceneDrawRecord> &records, const std::vector<float> &clip,
            uint64_t max_triangles, Begin begin, Vertex vertex, Triangle triangle)
        {
            MeshStats stats;
            bool full = false;
            std::unordered_map<Bits, int64_t, BitsHash> merged;
            for (const SceneDrawRecord &record : records)
            {
                if (!streamed(record) || record.written == 0)
                    continue;
                if (full || stats.triangles + stats.dropped + record.written > max_triangles)
                {
                    full = true;
                    stats.past_max += record.written;
                    continue;
                }
                begin(record);
                merged.clear();
                const size_t first = size_t(record.first_triangle) * 12;
                for (uint64_t t = 0; t < record.written && first + (t + 1) * 12 <= clip.size(); t++)
                {
                    int64_t corner[3];
                    for (int k = 0; k < 3; k++)
                    {
                        const float *c = &clip[first + t * 12 + k * 4];
                        Bits bits;
                        std::memcpy(bits.data(), c, sizeof bits);
                        const auto found = merged.find(bits);
                        if (found != merged.end())
                        {
                            corner[k] = found->second;
                            continue;
                        }
                        // -1 marks a corner at infinity, so every triangle that uses it is dropped.
                        int64_t index = -1;
                        if (const std::optional<Vec3d> world = clip_to_world(camera, { c[0], c[1], c[2], c[3] }))
                        {
                            index = vertex(Vec3d { (*world)[0] - origin[0], (*world)[1] - origin[1], -((*world)[2] - origin[2]) });
                            stats.vertices++;
                        }
                        merged.emplace(bits, index);
                        corner[k] = index;
                    }
                    if (corner[0] < 0 || corner[1] < 0 || corner[2] < 0)
                    {
                        stats.dropped++;
                        continue;
                    }
                    triangle(corner[0], corner[2], corner[1]);
                    stats.triangles++;
                }
            }
            return stats;
        }

        bool write_bytes(const fs::path &path, const void *data, size_t size)
        {
            std::ofstream out(path, std::ios::binary | std::ios::trunc);
            out.write(static_cast<const char *>(data), static_cast<std::streamsize>(size));
            return static_cast<bool>(out);
        }

        void write_text(std::vector<std::string> &failed, const fs::path &path, const std::string &text)
        {
            if (!write_bytes(path, text.data(), text.size()))
                failed.push_back(path.filename().u8string());
        }
    }

    MeshStats write_obj(std::ostream &out, const SceneCamera &camera, const Vec3d &origin, const std::vector<SceneDrawRecord> &records,
        const std::vector<float> &clip, uint64_t max_triangles)
    {
        out << "mtllib scene.mtl\n";
        int64_t count = 0;
        char line[128];
        return walk(camera, origin, records, clip, max_triangles,
            [&](const SceneDrawRecord &r) {
                const std::string stencil = r.stencil ? "_s" + std::to_string(r.stencil_ref) : "";
                const std::string domain = r.ds != 0 ? "_ds" + hex(r.ds) : "";
                out << "g d" << r.index << '_' << draw_kind(r) << stencil << "_vs" << hex(r.vs) << domain << "\nusemtl "
                    << kDrawClassNames[static_cast<size_t>(draw_class(r))] << '\n';
            },
            [&](const Vec3d &p) {
                const int length = std::snprintf(line, sizeof line, "v %.3f %.3f %.3f\n", p[0], p[1], p[2]);
                out.write(line, length);
                return ++count;
            },
            [&](int64_t a, int64_t b, int64_t c) {
                const int length = std::snprintf(line, sizeof line, "f %lld %lld %lld\n", static_cast<long long>(a), static_cast<long long>(b),
                    static_cast<long long>(c));
                out.write(line, length);
            });
    }

    MeshStats write_ply(std::ostream &out, const SceneCamera &camera, const Vec3d &origin, const std::vector<SceneDrawRecord> &records,
        const std::vector<float> &clip, uint64_t max_triangles)
    {
        // The header needs both counts first, so the body is built in memory.
        std::string vertices, faces;
        int64_t count = 0;
        const MeshStats stats = walk(camera, origin, records, clip, max_triangles, [](const SceneDrawRecord &) {},
            [&](const Vec3d &p) {
                const float xyz[3] = { float(p[0]), float(p[1]), float(p[2]) };
                vertices.append(reinterpret_cast<const char *>(xyz), sizeof xyz);
                return count++;
            },
            [&](int64_t a, int64_t b, int64_t c) {
                const int32_t corners[3] = { int32_t(a), int32_t(b), int32_t(c) };
                faces += '\3';
                faces.append(reinterpret_cast<const char *>(corners), sizeof corners);
            });
        out << "ply\nformat binary_little_endian 1.0\nelement vertex " << stats.vertices << "\nproperty float x\nproperty float y\nproperty float z\n"
            << "element face " << stats.triangles << "\nproperty list uchar int vertex_indices\nend_header\n";
        out.write(vertices.data(), static_cast<std::streamsize>(vertices.size()));
        out.write(faces.data(), static_cast<std::streamsize>(faces.size()));
        return stats;
    }

    std::string scene_mtl()
    {
        static constexpr const char *kColors[] = {
            "1 0 0", "0.5 0.5 0.5", "0.2 0.7 0.2", "0.9 0.2 0.9", "0.1 0.4 0.1", "0.5 0.9 0.4", "0.55 0.35 0.15", "0.55 0.35 0.15", "0.55 0.35 0.15",
        };
        static_assert(std::size(kColors) == static_cast<size_t>(DrawClass::count));
        std::string out = "# CameraTools Photoreal scene capture: one color per stencil class, as the stencil debug view shows them\n";
        for (size_t i = 0; i < kDrawClassNames.size(); i++)
            out += std::string("newmtl ") + kDrawClassNames[i] + "\nKd " + kColors[i] + "\n";
        return out;
    }

    std::vector<uint8_t> half_size(const std::vector<uint8_t> &rgb, Size size, Size *half)
    {
        const Size out_size = { std::max(size.width / 2, 1u), std::max(size.height / 2, 1u) };
        std::vector<uint8_t> out(size_t(out_size.width) * out_size.height * 3);
        for (uint32_t y = 0; y < out_size.height; y++)
            for (uint32_t x = 0; x < out_size.width; x++)
                for (int c = 0; c < 3; c++)
                {
                    uint32_t sum = 0, n = 0;
                    for (uint32_t dy = 0; dy < 2; dy++)
                        for (uint32_t dx = 0; dx < 2; dx++)
                        {
                            const uint32_t sx = x * 2 + dx, sy = y * 2 + dy;
                            if (sx < size.width && sy < size.height)
                            {
                                sum += rgb[(size_t(sy) * size.width + sx) * 3 + c];
                                n++;
                            }
                        }
                    out[(size_t(y) * out_size.width + x) * 3 + c] = static_cast<uint8_t>((sum + n / 2) / n);
                }
        *half = out_size;
        return out;
    }

    std::string mesh_name(uint32_t face, bool ply)
    {
        return "face-" + std::to_string(face) + "-" + kFaces[face].name + (ply ? ".ply" : ".obj");
    }

    const char *const kSceneHeader = "face\tname\tframe\tattempts\tcamera\tcamera_note\tstatus\twidth\theight\tdraws\treplayed"
        "\tskip_no-bytecode\tskip_gs-bound\tskip_no-position\tskip_game-so\tskip_query-pool\tskip_gs-create-failed\tskip_topology"
        "\tskip_buffer-full\twritten\tneeded\toverflowed\tfile_triangles\tfile_vertices\tdropped\tpast_max\tunwritten_draws\tunwritten_corners\toverran_draws"
        "\tpre_tested\tpre_matched\tpre_in_front\tpre_behind\ts128_tested\ts128_matched\ts128_in_front\ts128_behind"
        "\ts129_tested\ts129_matched\ts129_in_front\ts129_behind\ts133_tested\ts133_matched\ts133_in_front\ts133_behind"
        "\ts136_tested\ts136_matched\ts136_in_front\ts136_behind\ts137_tested\ts137_matched\ts137_in_front\ts137_behind"
        "\ts144_tested\ts144_matched\ts144_in_front\ts144_behind\ts165_tested\ts165_matched\ts165_in_front\ts165_behind"
        "\tother_tested\tother_matched\tother_in_front\tother_behind\toff_screen\torigin\tposition\tforward\tup\n";

    const char *const kDrawsHeader = "face\tdraw\tkind\tclass\tcall\tcount\tinstances\tfirst\tbase_vertex\tfirst_instance\targs_offset\ttopology"
        "\tvs\tds\tps\tstencil\tstencil_ref\tdepth_func\tdepth_write\texpected\twritten\tneeded\tfirst_triangle\tskip\tpoints\toffset\treserved\tcorners\tunwritten"
        "\tt0\tt1\tt2\tt3\tt4\tt5\tt6\tt7\n";

    const char *const kCameraHeader = "face\tsource\tb1_5\tb1_10\tb2_6\tb2_7\tb2_8\tb2_9\tb2_10\tb2_11\tb2_12\tb2_13\tb2_18\tb2_19\tb2_20"
        "\tclip_to_world\tworld_to_view\tview_to_clip\n";

    std::string scene_row(const SceneFaceData &d, const MeshStats &mesh, const DepthCheck &depth)
    {
        std::array<uint32_t, static_cast<size_t>(DrawSkip::count)> skips {};
        uint64_t written = 0, needed = 0, unwritten = 0;
        uint32_t unwritten_draws = 0, overran_draws = 0;
        for (const SceneDrawRecord &r : d.records)
        {
            skips[static_cast<size_t>(r.skip)]++;
            written += r.written;
            needed += r.needed;
            unwritten += r.unwritten;
            unwritten_draws += r.unwritten != 0;
            overran_draws += r.skip == DrawSkip::none && overran(r);
        }
        std::string row = std::to_string(d.face) + '\t' + kFaces[d.face].name + '\t' + std::to_string(d.frame) + '\t' + std::to_string(d.attempts) + '\t'
            + kCameraSourceNames[static_cast<size_t>(d.camera.source)] + '\t' + (d.camera_note.empty() ? "-" : d.camera_note) + '\t' + d.status + '\t'
            + std::to_string(d.render.width) + '\t' + std::to_string(d.render.height) + '\t' + std::to_string(d.records.size()) + '\t'
            + std::to_string(skips[0]);
        for (size_t i = 1; i < skips.size(); i++)
            row += '\t' + std::to_string(skips[i]);
        row += '\t' + std::to_string(written) + '\t' + std::to_string(needed) + '\t' + (d.overflowed ? "1" : "0") + '\t' + std::to_string(mesh.triangles)
            + '\t' + std::to_string(mesh.vertices) + '\t' + std::to_string(mesh.dropped) + '\t' + std::to_string(mesh.past_max) + '\t'
            + std::to_string(unwritten_draws) + '\t' + std::to_string(unwritten) + '\t' + std::to_string(overran_draws);
        for (const DepthCounts &c : depth.classes)
            row += '\t' + std::to_string(c.tested) + '\t' + std::to_string(c.matched) + '\t' + std::to_string(c.in_front) + '\t' + std::to_string(c.behind);
        row += '\t' + std::to_string(depth.off_screen) + '\t' + vec(d.origin) + '\t' + vec(d.camera.position) + '\t' + vec(d.camera.basis[2]) + '\t'
            + vec(d.camera.basis[1]) + '\n';
        return row;
    }

    std::string draw_rows(uint32_t face, const std::vector<SceneDrawRecord> &records)
    {
        std::string out;
        for (const SceneDrawRecord &r : records)
        {
            const bool direct = r.kind == ReplayKind::draw || r.kind == ReplayKind::indexed;
            const auto number = [direct](uint64_t v) { return direct ? std::to_string(v) : std::string("-"); };
            const std::optional<uint64_t> expected = expected_triangles(r);
            out += std::to_string(face) + '\t' + std::to_string(r.index) + '\t' + draw_kind(r) + '\t' + kDrawClassNames[static_cast<size_t>(draw_class(r))]
                + '\t' + kReplayKindNames[static_cast<size_t>(r.kind)] + '\t' + number(r.count) + '\t' + number(r.instances) + '\t' + number(r.first)
                + '\t' + (direct ? std::to_string(r.base_vertex) : "-") + '\t' + number(r.first_instance) + '\t'
                + (direct ? std::string("-") : std::to_string(r.args_offset)) + '\t' + std::to_string(r.topology) + '\t' + hex(r.vs) + '\t' + hex(r.ds)
                + '\t' + hex(r.ps) + '\t' + (r.stencil ? "1" : "0") + '\t' + std::to_string(r.stencil_ref) + '\t'
                + (r.depth_func < std::size(kDepthFuncNames) ? kDepthFuncNames[r.depth_func] : "?") + '\t' + (r.depth_write ? "1" : "0") + '\t'
                + (expected ? std::to_string(*expected) : "-") + '\t' + std::to_string(r.written) + '\t' + std::to_string(r.needed) + '\t'
                + std::to_string(r.first_triangle) + '\t' + kDrawSkipNames[static_cast<size_t>(r.skip)] + '\t' + (r.points ? "1" : "0") + '\t'
                + std::to_string(r.offset) + '\t' + std::to_string(r.reserved) + '\t' + std::to_string(r.corners) + '\t' + std::to_string(r.unwritten);
            for (const Texture &t : r.ps_inputs)
            {
                if (t.id == ResourceId {})
                {
                    out += "\t-";
                    continue;
                }
                char cell[64];
                std::snprintf(cell, sizeof cell, "\t%llx:%u:%ux%u", static_cast<unsigned long long>(t.id), static_cast<uint32_t>(t.format), t.size.width,
                    t.size.height);
                out += cell;
            }
            out += '\n';
        }
        return out;
    }

    std::string camera_row(const SceneFaceData &d)
    {
        std::string row = std::to_string(d.face) + '\t' + kCameraSourceNames[static_cast<size_t>(d.camera.source)];
        // b1 starts 768 bytes into SunCbuffers and b2 192 bytes after it.
        const auto sun_row = [&d](size_t buffer_offset, uint32_t index) {
            float f[4];
            std::memcpy(f, d.sun->data() + buffer_offset + index * 16, sizeof f);
            const double values[4] = { f[0], f[1], f[2], f[3] };
            return '\t' + joined(values, 4);
        };
        if (d.sun)
        {
            row += sun_row(768, 5) + sun_row(768, 10);
            for (uint32_t i = 6; i <= 13; i++)
                row += sun_row(960, i);
            for (uint32_t i = 18; i <= 20; i++)
                row += sun_row(960, i);
        }
        else
            for (int i = 0; i < 13; i++)
                row += "\t-";
        row += '\t' + joined(d.camera.clip_to_world.data(), 16);
        if (d.pushed)
        {
            // Row-major like clip_to_world: Unity's arrays are column-major.
            double view[16], projection[16];
            for (int r = 0; r < 4; r++)
                for (int c = 0; c < 4; c++)
                {
                    view[r * 4 + c] = d.pushed->world_to_view[c * 4 + r];
                    projection[r * 4 + c] = d.pushed->view_to_clip[c * 4 + r];
                }
            row += '\t' + joined(view, 16) + '\t' + joined(projection, 16);
        }
        else
            row += "\t-\t-";
        return row + '\n';
    }

    std::vector<std::string> begin_scene_folder(const SceneFolder &folder)
    {
        std::vector<std::string> failed;
        std::error_code error;
        fs::create_directories(folder.path, error);
        write_text(failed, folder.path / "scene.mtl", scene_mtl());
        return failed;
    }

    std::vector<std::string> write_scene_face(SceneFolder &folder, const SceneFaceData &d, const SceneRequest &request)
    {
        std::vector<std::string> failed;
        const std::string prefix = "face-" + std::to_string(d.face);
        MeshStats mesh;
        {
            const fs::path path = folder.path / mesh_name(d.face, request.ply);
            std::ofstream out(path, std::ios::binary | std::ios::trunc);
            mesh = request.ply ? write_ply(out, d.camera, d.origin, d.records, d.clip, request.max_triangles)
                               : write_obj(out, d.camera, d.origin, d.records, d.clip, request.max_triangles);
            if (!out)
                failed.push_back(path.filename().u8string());
        }
        if (!d.rgb.empty() && d.picture.width != 0 && d.picture.height != 0)
        {
            Size half;
            const std::vector<uint8_t> small = half_size(d.rgb, d.picture, &half);
            write_text(failed, folder.path / (prefix + ".png"), encode_png(small.data(), half.width, half.height));
        }
        if (!d.depth.empty() && !write_bytes(folder.path / (prefix + "-depth.r32"), d.depth.data(), d.depth.size() * sizeof(float)))
            failed.push_back(prefix + "-depth.r32");
        if (request.raw && !write_bytes(folder.path / (prefix + ".so"), d.clip.data(), d.clip.size() * sizeof(float)))
            failed.push_back(prefix + ".so");
        if (d.sun && !write_bytes(folder.path / (prefix + "-sun.bin"), d.sun->data(), d.sun->size()))
            failed.push_back(prefix + "-sun.bin");

        folder.faces.push_back(scene_row(d, mesh, check_depth(d.camera, d.records, d.clip, d.render, d.depth)));
        folder.draws += draw_rows(d.face, d.records);
        folder.cameras.push_back(camera_row(d));
        std::string scene = kSceneHeader, cameras = kCameraHeader;
        for (const std::string &row : folder.faces)
            scene += row;
        for (const std::string &row : folder.cameras)
            cameras += row;
        write_text(failed, folder.path / "scene.tsv", scene);
        write_text(failed, folder.path / "draws.tsv", kDrawsHeader + folder.draws);
        write_text(failed, folder.path / "camera.tsv", cameras);
        return failed;
    }

    std::vector<std::string> write_scene_stopped(const SceneFolder &folder, const std::string &note)
    {
        std::vector<std::string> failed;
        std::error_code error;
        fs::create_directories(folder.path, error);
        write_text(failed, folder.path / "stopped.txt", note + "\n");
        return failed;
    }
}
