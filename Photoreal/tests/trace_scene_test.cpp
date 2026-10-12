// The traced scene: triangle keys on the 1 mm grid, which copy of a triangle is kept, grass left out, the cap on
// triangles, a face's clip-space corners into the reference's frame, the self-test scene, trace-scene.bin, and the
// counts of fixtures/scene-194351-near.bin, the triangles of build 199's scene capture within 10 m of its camera, which
// make_trace_fixture.py wrote and counted; the alpha records of alpha-tested triangles, and their buffer in the file;
// and fixtures/scene-161003-meadow-rays.bin, rays of build 207's meadow frame that met their work cap with the triangles
// they cross, which make_meadow_fixture.cpp cut: the equal-depth copies without a twin are left out, and no ray meets a cap.
#include "../core/bvh.h"
#include "../core/trace_scene.h"

#include <algorithm>

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

    void check(const char *name, bool ok)
    {
        std::printf("%s %s\n", ok ? "PASS" : "FAIL", name);
        failures += ok ? 0 : 1;
    }

    std::string slurp(const std::string &path)
    {
        std::ifstream file(path, std::ios::binary);
        std::string data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        if (data.empty())
            throw std::runtime_error("missing " + path);
        return data;
    }

    using Tri = std::array<Vec3d, 3>;

    // A draw record of a kind and class, as SceneGpu::replay records the game's depth-stencil state.
    SceneDrawRecord draw(const char *kind, uint32_t stencil)
    {
        SceneDrawRecord r;
        if (std::strcmp(kind, "pre") == 0)
        {
            r.depth_write = true;
            r.depth_func = 7;
            return r;
        }
        r.stencil = true;
        r.stencil_ref = stencil;
        r.depth_func = std::strcmp(kind, "equal") == 0 ? 3 : 7;
        r.depth_write = std::strcmp(kind, "main") == 0;
        return r;
    }

    const Tri kA = { Vec3d { 0, 0, 0 }, Vec3d { 1, 0, 0 }, Vec3d { 0, 1, 0 } };
    const Tri kB = { Vec3d { 5, 0, 0 }, Vec3d { 6, 0, 0 }, Vec3d { 5, 1, 0 } };
    const Tri kC = { Vec3d { 0, 0, 3 }, Vec3d { 1, 0, 3 }, Vec3d { 0, 1, 3 } };

    struct FixtureTriangle
    {
        Tri corners;
        uint8_t face, kind, cls;
    };

    // A uint32 count at offset, then 40-byte records (nine float32, face, kind, class, 0).
    std::vector<FixtureTriangle> load_triangles(const std::string &bytes, size_t offset)
    {
        uint32_t count = 0;
        std::memcpy(&count, bytes.data() + offset, 4);
        std::vector<FixtureTriangle> out(count);
        for (uint32_t i = 0; i < count; i++)
        {
            const char *at = bytes.data() + offset + 4 + size_t(i) * 40;
            float f[9];
            std::memcpy(f, at, sizeof f);
            for (int c = 0; c < 3; c++)
                out[i].corners[c] = { f[c * 3], f[c * 3 + 1], f[c * 3 + 2] };
            out[i].face = static_cast<uint8_t>(at[36]);
            out[i].kind = static_cast<uint8_t>(at[37]);
            out[i].cls = static_cast<uint8_t>(at[38]);
        }
        return out;
    }

    // fixtures/scene-194351-near.bin: b"PTF1", then the triangles.
    std::vector<FixtureTriangle> load_fixture(const std::string &path) { return load_triangles(slurp(path), 4); }

    struct FixtureRay
    {
        Vec3f origin, direction;
        float t_max;
        uint32_t any_hit, classes;
    };

    // fixtures/scene-161003-meadow-rays.bin: b"PMF1", a uint32 ray count, 36 bytes a ray, then the triangles.
    void load_meadow(const std::string &path, std::vector<FixtureRay> &rays, std::vector<FixtureTriangle> &triangles)
    {
        const std::string bytes = slurp(path);
        uint32_t count = 0;
        std::memcpy(&count, bytes.data() + 4, 4);
        rays.resize(count);
        for (uint32_t i = 0; i < count; i++)
            std::memcpy(&rays[i], bytes.data() + 8 + size_t(i) * 36, 36);
        triangles = load_triangles(bytes, 8 + size_t(count) * 36);
    }
    static_assert(sizeof(FixtureRay) == 36);

    // The record make_trace_fixture.py's kind and class stand for.
    SceneDrawRecord fixture_draw(uint8_t kind, uint8_t cls)
    {
        static const uint32_t kStencils[] = { 0, 128, 129, 133, 136, 137, 144, 165, 200 };
        static const char *const kKinds[] = { "pre", "equal", "main" };
        return draw(kKinds[kind], kStencils[cls]);
    }
}

int main()
{
    {
        const std::optional<TriangleKey> key = triangle_key(kA);
        const std::optional<TriangleKey> rotated = triangle_key({ kA[1], kA[2], kA[0] });
        const std::optional<TriangleKey> flipped = triangle_key({ kA[0], kA[2], kA[1] });
        check("a triangle's key does not depend on its corner order", key && rotated && flipped && *key == *rotated && *key == *flipped);
        check("its key is its corners in millimeters, sorted", key && key->q == std::array<int32_t, 9> { 0, 0, 0, 0, 1000, 0, 1000, 0, 0 });
        const std::optional<TriangleKey> moved = triangle_key({ Vec3d { 0.001, 0, 0 }, kA[1], kA[2] });
        const std::optional<TriangleKey> near = triangle_key({ Vec3d { 0.00049, 0, 0 }, kA[1], kA[2] });
        check("a corner 1 mm away makes another key, and 0.49 mm away the same", moved && !(*moved == *key) && near && *near == *key);
        check("a triangle whose corners merge on the grid has no key", !triangle_key({ kA[0], Vec3d { 0.0004, 0, 0 }, kA[2] }));
        check("a corner at infinity, NaN or 3000 km away has no key", !triangle_key({ Vec3d { INFINITY, 0, 0 }, kA[1], kA[2] })
            && !triangle_key({ Vec3d { NAN, 0, 0 }, kA[1], kA[2] }) && !triangle_key({ Vec3d { 3e6, 0, 0 }, kA[1], kA[2] }));
    }
    {
        SceneBuilder b;
        b.add_draw(draw("pre", 0), { kA, kB });
        b.add_draw(draw("main", 136), { kA });
        b.add_draw(draw("equal", 137), { kA, kB });
        const SceneBuilder::Counts c = b.counts();
        const std::vector<TraceTriangle> t = b.take(100);
        check("a main copy replaces the prepass's, and an equal copy the prepass's but not the main's", c.in == 5 && c.unique == 2 && c.replaced == 2
            && c.pre_only == 0 && t.size() == 2 && info_class(t[0].info) == TraceClass::vegetation && info_class(t[1].info) == TraceClass::foliage);
        SceneBuilder first;
        first.add_draw(draw("main", 128), { kA });
        first.add_draw(draw("main", 133), { { kA[2], kA[0], kA[1] } });
        first.add_draw(draw("pre", 0), { kC });
        const std::vector<TraceTriangle> kept = first.take(100);
        check("of two main copies the first face's is kept, and a prepass triangle without a twin stays as other", first.counts().replaced == 0
            && first.counts().pre_only == 1 && kept.size() == 2 && info_class(kept[0].info) == TraceClass::world
            && kept[0].corner[0] == Vec3f { 0, 0, 0 } && info_class(kept[1].info) == TraceClass::other);
    }
    {
        SceneBuilder b;
        b.add_draw(draw("main", 129), { kA, kB });
        b.add_draw(draw("main", 165), { kC, { kA[0], kA[0], kA[1] } });
        const SceneBuilder::Counts c = b.counts();
        check("grass is left out and a degenerate triangle dropped; a character casts", c.in == 4 && c.dropped_class == 2 && c.dropped_degenerate == 1
            && c.unique == 1 && info_class(b.take(10)[0].info) == TraceClass::character);
        SceneDrawRecord volume = draw("main", 38);
        volume.depth_func = 8;
        volume.depth_write = false;
        SceneDrawRecord overlay = draw("main", 133);
        overlay.depth_write = false;
        SceneBuilder none;
        none.add_draw(volume, { kA });
        none.add_draw(overlay, { kB });
        none.add_draw(draw("pre", 0), { kC });
        none.add_draw(draw("equal", 137), { kC });
        check("draws that write no depth cast nothing, but an equal-depth draw does", none.counts().dropped_no_depth == 2 && none.counts().unique == 1
            && none.counts().replaced == 1 && info_class(none.take(10)[0].info) == TraceClass::foliage && !leaves_depth(volume)
            && leaves_depth(draw("equal", 136)) && leaves_depth(draw("pre", 0)));
        // An EQUAL depth test passes only on a surface an earlier draw laid down.
        SceneBuilder alone;
        alone.add_draw(draw("equal", 136), { kA });
        alone.add_draw(draw("main", 128), { kB });
        alone.add_draw(draw("equal", 136), { kB, kC });
        alone.add_draw(draw("pre", 0), { kA });
        const SceneBuilder::Counts lone = alone.counts();
        const std::vector<TraceTriangle> left = alone.take(10);
        check("an equal-depth copy with no earlier twin is left out, whatever comes later, and one of a main copy changes nothing",
            lone.in == 5 && lone.dropped_equal_alone == 2 && lone.unique == 2 && lone.replaced == 0 && lone.pre_only == 1 && left.size() == 2
                && info_class(left[0].info) == TraceClass::world && info_class(left[1].info) == TraceClass::other);
        check("the classes map from their stencils", trace_class(draw("main", 144)) == TraceClass::world
            && trace_class(draw("main", 200)) == TraceClass::other && trace_class(draw("pre", 0)) == TraceClass::other);
        SceneDrawRecord tall = draw("main", 137), leaf = draw("main", 137);
        AlphaAnalysis grass_shader, leaf_shader;
        grass_shader.material = kTallGrassMaterial;
        leaf_shader.material = 2;
        tall.analysis = std::make_shared<const AlphaAnalysis>(grass_shader);
        leaf.analysis = std::make_shared<const AlphaAnalysis>(leaf_shader);
        SceneBuilder meadow;
        meadow.add_draw(tall, { kA });
        meadow.add_draw(leaf, { kB });
        check("tall grass under the foliage stencil, a pixel shader writing material 3, is left out like stencil 129; leaves stay foliage",
            !trace_class(tall) && trace_class(leaf) == TraceClass::foliage && meadow.counts().dropped_class == 1 && meadow.counts().unique == 1);
    }
    {
        SceneBuilder b;
        const Tri far = { Vec3d { 50, 0, 0 }, Vec3d { 51, 0, 0 }, Vec3d { 50, 1, 0 } };
        b.add_draw(draw("main", 128), { kB, far, kA, kC });
        const std::vector<TraceTriangle> t = b.take(2);
        check("past the cap the farthest triangles go first, and the rest keep their order", b.counts().past_max == 2 && t.size() == 2
            && t[0].corner[0] == Vec3f { 0, 0, 0 } && t[1].corner[0] == Vec3f { 0, 0, 3 });
    }
    {
        // A camera whose clip space is the world 10 m along x, homogeneous, with w scaling everything.
        SceneCamera camera;
        camera.clip_to_world = { 1, 0, 0, 10, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
        SceneDrawRecord r = draw("main", 128);
        r.skip = DrawSkip::none;
        r.written = 2;
        r.first_triangle = 0;
        r.reserved = r.corners = 6;
        const std::vector<float> clip = { 0, 0, 0, 1, 2, 0, 0, 2, 0, 3, 0, 1, 0, 0, 0, 1, 1, 0, 0, 1, 0, 1, 0, 0 };
        SceneBuilder b;
        add_face(b, camera, { 10, 0, 0 }, { r }, clip);
        const std::vector<TraceTriangle> t = b.take(10);
        check("a face's corners go to the world through its camera and become relative to the origin; a corner at infinity drops its triangle",
            b.counts().in == 2 && b.counts().dropped_degenerate == 1 && t.size() == 1 && t[0].corner[0] == Vec3f { 0, 0, 0 }
            && t[0].corner[1] == Vec3f { 1, 0, 0 } && t[0].corner[2] == Vec3f { 0, 3, 0 });
    }
    {
        const std::vector<TraceTriangle> t = self_test_scene({ 0, 0, 1 });
        check("the self-test scene: a roof 4 m up, a wall 5 m ahead and a plate 1 m down", t.size() == 3 && t[0].corner[2] == Vec3f { 0, 4, 14 }
            && t[1].corner[0] == Vec3f { -2, -3, 5 } && t[1].corner[2] == Vec3f { 0, 1, 5 } && t[2].corner[2] == Vec3f { 0, -1, 3 }
            && info_class(t[2].info) == TraceClass::world);
        const std::string bytes = trace_scene_bytes(t, { 1776.5, 0.25, -390.75 });
        const std::optional<TraceSceneFile> back = parse_trace_scene(bytes);
        check("trace-scene.bin holds the triangles and the reference, 32 + 40 bytes each", bytes.size() == 32 + 3 * 40 && back
            && back->reference == Vec3d { 1776.5, 0.25, -390.75 } && back->triangles.size() == 3 && back->triangles[1].corner == t[1].corner
            && back->triangles[1].info == t[1].info && !parse_trace_scene(bytes.substr(0, bytes.size() - 1)));
    }
    {
        // Alpha tests: a prepass copy of kA with leaves' alpha test, a main copy of it that replaces it with another, a
        // whole world triangle kB, and kC alpha-tested; each alpha triangle's corners hold (u, v) and the main copy's 10 more.
        std::ifstream file("fixtures/68b05754fdce207c.ps.dxbc", std::ios::binary);
        const std::vector<uint8_t> code((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        const AlphaAnalysis cutoff = analyze_alpha(code.data(), code.size());
        const AlphaDraw pre_alpha { cutoff.program, 2, 1 | 1 << 3, { 1, 2, 3, 4 } }, main_alpha { cutoff.program, 5, 3 | 3 << 3, { 5, 6, 7, 8 } };
        const float pre_uv[6] = { 0, 0, 1, 0, 0, 1 };
        const float main_uv[12] = { 10, 10, 11, 10, 10, 11, 0, 0, 1, 0, 0, 1 };
        SceneBuilder b;
        b.add_draw(draw("pre", 0), { kA }, &pre_alpha, pre_uv);
        b.add_draw(draw("main", 128), { kB });
        b.add_draw(draw("main", 137), { kA, kC }, &main_alpha, main_uv);
        b.set_atlas(std::vector<uint8_t>(kAtlasTile * kAtlasTile * 6, 7));
        std::vector<uint32_t> alpha;
        const std::vector<TraceTriangle> t = b.take(10, &alpha);
        // The buffer: 4 header words, the program, one draw block (the main copy's: the prepass copy is no longer kept)
        // with its row, then two records of 1 + 3 x 2 words, then 6 tiles.
        const uint32_t program_words = static_cast<uint32_t>(cutoff.program->words().size());
        check("the kept alpha triangles get records in the order they arrive, the replaced prepass copy none, the whole one none",
            t.size() == 3 && b.counts().alpha == 2 && (t[0].info & kOpaque) == 0 && info_class(t[0].info) == TraceClass::foliage
                && (t[1].info & kOpaque) == kOpaque && (t[2].info & kOpaque) == 7 && alpha.size() == 4 + program_words + 8 + 14 + 6 * 16384
                && alpha[0] == 4 + program_words + 8 && alpha[1] == 4 && alpha[2] == 4 + program_words && alpha[3] == 4 + program_words + 8 + 14);
        const uint32_t draws = alpha[2], records = alpha[0];
        float first_u;
        std::memcpy(&first_u, &alpha[records + 1], 4);
        check("the main copy's block holds its tile, address modes and row, and its record its own corners", alpha[draws] == 0 && alpha[draws + 1] == 5
            && alpha[draws + 2] == (3 | 3 << 3) && alpha[draws + 3] == 1 && alpha[draws + 4] == 5 && alpha[draws + 7] == 8 && alpha[records] == 0
            && first_u == 10 && alpha[alpha[3]] == 0x07070707u);
        SceneBuilder equal;
        equal.add_draw(draw("pre", 0), { kA }, &pre_alpha, pre_uv);
        equal.add_draw(draw("equal", 136), { { kA[1], kA[2], kA[0] } });
        std::vector<uint32_t> inherited;
        const std::vector<TraceTriangle> e = equal.take(10, &inherited);
        float u1;
        std::memcpy(&u1, &inherited[inherited[0] + 1 + 2], 4);
        check("an equal-depth copy without an alpha test keeps the prepass copy's, its class its own and its corners the prepass's order",
            e.size() == 1 && info_class(e[0].info) == TraceClass::vegetation && (e[0].info & kOpaque) == 0 && equal.counts().inherited == 1
                && e[0].corner[0] == Vec3f { 0, 0, 0 } && inherited[inherited[2] + 1] == 2 && u1 == 1);
        // A face's draw with a program: its corners' floats from the stream's attributes at its first_attribute, its rows
        // from the frame's constants at alpha_constants, as unpack_stream and the replay's copies left them.
        SceneCamera camera;
        camera.clip_to_world = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
        SceneDrawRecord r = draw("main", 137);
        r.written = 1;
        r.reserved = r.corners = 3;
        r.alpha = DrawAlpha::program;
        r.analysis = std::make_shared<const AlphaAnalysis>(cutoff);
        r.stride = 24;
        r.first_attribute = 2;
        r.alpha_tile = 4;
        r.alpha_address = 9;
        r.alpha_constants = 4;
        r.alpha_rows = 1;
        const std::vector<float> clip = { 0, 0, 0, 1, 1, 0, 0, 1, 0, 1, 0, 1 }, attributes = { 9, 9, 0.5f, 0.25f, 1, 0.25f, 0.5f, 1 };
        const std::vector<uint32_t> constants = { 0, 0, 0, 0, 11, 12, 13, 14 };
        SceneBuilder face;
        add_face(face, camera, { 0, 0, 0 }, { r }, clip, attributes, constants);
        std::vector<uint32_t> streamed;
        const std::vector<TraceTriangle> f = face.take(10, &streamed);
        float u0;
        std::memcpy(&u0, &streamed[streamed[0] + 1], 4);
        check("a face's draw with a program brings its corners' floats from its first attribute and its row from its constants",
            f.size() == 1 && (f[0].info & kOpaque) == 0 && streamed[streamed[2] + 1] == 4 && streamed[streamed[2] + 2] == 9 && streamed[streamed[2] + 4] == 11
                && streamed[streamed[2] + 7] == 14 && u0 == 0.5f);
        r.stride = 16;
        SceneBuilder short_stride;
        add_face(short_stride, camera, { 0, 0, 0 }, { r }, clip, attributes, constants);
        std::vector<uint32_t> none_streamed;
        short_stride.take(10, &none_streamed);
        check("a draw whose stride does not hold its program's floats stays whole", none_streamed.empty());
        SceneBuilder plain;
        plain.add_draw(draw("main", 128), { kA });
        std::vector<uint32_t> none = { 1 };
        plain.take(10, &none);
        check("a scene without alpha tests gives no alpha buffer", none.empty() && plain.counts().alpha == 0);
        const std::string bytes = trace_scene_bytes(t, { 1, 2, 3 }, alpha);
        const std::optional<TraceSceneFile> back = parse_trace_scene(bytes);
        check("trace-scene.bin with an alpha buffer: its word count and words after the triangles, read back whole",
            bytes.size() == 32 + 3 * 40 + 4 + alpha.size() * 4 && back && back->alpha == alpha && back->triangles[2].info == t[2].info
                && !parse_trace_scene(bytes.substr(0, bytes.size() - 4)) && !parse_trace_scene(bytes.substr(0, 32 + 3 * 40 + 2)));
    }
    {
        // Values from make_trace_fixture.py's own deduplication of the same triangles.
        const std::vector<FixtureTriangle> fixture = load_fixture("fixtures/scene-194351-near.bin");
        SceneBuilder b;
        std::vector<Tri> run;
        for (size_t i = 0; i < fixture.size(); i++)
        {
            run.push_back(fixture[i].corners);
            const bool last = i + 1 == fixture.size() || fixture[i + 1].face != fixture[i].face || fixture[i + 1].kind != fixture[i].kind
                || fixture[i + 1].cls != fixture[i].cls;
            if (!last)
                continue;
            b.add_draw(fixture_draw(fixture[i].kind, fixture[i].cls), run);
            run.clear();
        }
        const SceneBuilder::Counts c = b.counts();
        std::printf("INFO fixture: in %llu, grass %llu, degenerate %llu, unique %llu, replaced %llu, prepass only %llu\n",
            static_cast<unsigned long long>(c.in), static_cast<unsigned long long>(c.dropped_class), static_cast<unsigned long long>(c.dropped_degenerate),
            static_cast<unsigned long long>(c.unique), static_cast<unsigned long long>(c.replaced), static_cast<unsigned long long>(c.pre_only));
        check("the fixture's counts match make_trace_fixture.py's", c.in == 48894 && c.dropped_class == 5158 && c.dropped_degenerate == 392
            && c.unique == 14165 && c.replaced == 539 && c.pre_only == 1346);
    }
    {
        // Build 207's meadow (scene-20261012-161003, frame trace-20261012-161041): the 24 occlusion rays of one tile that
        // did the most work, and the triangles along them. Every equal-depth draw of that capture came back in its
        // prepass draw's layout, so its triangles are fans from one real corner to two misread ones beside the camera.
        std::vector<FixtureRay> rays;
        std::vector<FixtureTriangle> fixture;
        load_meadow("fixtures/scene-161003-meadow-rays.bin", rays, fixture);
        // What the rays do in the scene a builder makes of the triangles, with the equal-depth copies as kind says.
        struct Run
        {
            SceneBuilder::Counts counts;
            size_t triangles = 0;
            uint32_t capped = 0, most_work = 0, hits = 0;
            bool as_brute = true;
        };
        const auto run = [&](const char *equal_as) {
            static const uint32_t kStencils[] = { 0, 128, 129, 133, 136, 137, 144, 165, 200 };
            static const char *const kKinds[] = { "pre", "equal", "main" };
            SceneBuilder b;
            for (const FixtureTriangle &t : fixture)
                b.add_draw(draw(t.kind == 1 ? equal_as : kKinds[t.kind], kStencils[t.cls]), { t.corners });
            Run out;
            out.counts = b.counts();
            const std::vector<TraceTriangle> scene = b.take(1u << 20);
            out.triangles = scene.size();
            const Bvh bvh = build_bvh(scene);
            for (const FixtureRay &r : rays)
            {
                TraversalStats stats;
                const std::optional<Hit> hit = trace_nearest(bvh, r.origin, r.direction, r.t_max, r.classes, &stats);
                const std::optional<Hit> brute = brute_nearest(bvh, r.origin, r.direction, r.t_max, r.classes);
                out.capped += stats.guard;
                out.most_work = std::max(out.most_work, stats.work);
                out.hits += hit.has_value();
                out.as_brute &= hit.has_value() == brute.has_value() && (!hit || (hit->triangle == brute->triangle && hit->t == brute->t));
            }
            return out;
        };
        const Run kept = run("main"), fixed = run("equal");
        std::printf("INFO meadow fixture: %zu rays, %zu triangles; with the equal-depth copies kept %zu triangles, %u rays capped, most work %u; "
                    "left out %llu, %zu triangles, %u rays capped, most work %u, %u hits\n",
            rays.size(), fixture.size(), kept.triangles, kept.capped, kept.most_work, static_cast<unsigned long long>(fixed.counts.dropped_equal_alone),
            fixed.triangles, fixed.capped, fixed.most_work, fixed.hits);
        check("the meadow fixture's rays all meet their work cap when the equal-depth copies without a twin are kept, as build 207 kept them",
            rays.size() == 24 && fixture.size() == 7262 && kept.triangles == 7262 && kept.capped == 24);
        check("the scene build leaves the meadow fixture's 7,046 equal-depth copies without a twin out", fixed.counts.in == 7262
            && fixed.counts.dropped_equal_alone == 7046 && fixed.counts.unique == 216 && fixed.triangles == 216);
        check("then no ray of the fixture meets a cap, none does more than 100 work, and each meets what testing every triangle meets",
            fixed.capped == 0 && fixed.most_work <= 100 && fixed.as_brute);
    }
    std::printf("%s: %d failed\n", __FILE__, failures);
    return failures == 0 ? 0 : 1;
}
