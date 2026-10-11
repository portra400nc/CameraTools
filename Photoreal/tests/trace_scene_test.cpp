// The traced scene: triangle keys on the 1 mm grid, which copy of a triangle is kept, grass left out, the cap on
// triangles, a face's clip-space corners into the reference's frame, the self-test scene, trace-scene.bin, and the
// counts of fixtures/scene-194351-near.bin, the triangles of build 199's scene capture within 10 m of its camera, which
// make_trace_fixture.py wrote and counted.
#include "../core/trace_scene.h"

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

    // fixtures/scene-194351-near.bin: b"PTF1", a uint32 count, then 40-byte records (nine float32, face, kind, class, 0).
    std::vector<FixtureTriangle> load_fixture(const std::string &path)
    {
        const std::string bytes = slurp(path);
        uint32_t count = 0;
        std::memcpy(&count, bytes.data() + 4, 4);
        std::vector<FixtureTriangle> out(count);
        for (uint32_t i = 0; i < count; i++)
        {
            const char *at = bytes.data() + 8 + size_t(i) * 40;
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
        none.add_draw(draw("equal", 137), { kC });
        check("draws that write no depth cast nothing, but an equal-depth draw does", none.counts().dropped_no_depth == 2 && none.counts().unique == 1
            && !leaves_depth(volume) && leaves_depth(draw("equal", 136)) && leaves_depth(draw("pre", 0)));
        check("the classes map from their stencils", trace_class(draw("main", 144)) == TraceClass::world
            && trace_class(draw("main", 200)) == TraceClass::other && trace_class(draw("pre", 0)) == TraceClass::other);
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
    std::printf("%s: %d failed\n", __FILE__, failures);
    return failures == 0 ? 0 : 1;
}
