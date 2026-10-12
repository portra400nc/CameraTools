// The trace pass's decisions and its kernel compiled as C++: the ray generator's seeds, sequence and directions, the
// samplers' means, the tiles' Morton order, the schedule of tiles and the time budget, what starts the samples again,
// the sun's direction and floating origin from build 199's view-face constants (fixtures/scene-194351-face-6-sun.bin),
// the constants, one dispatch's work on the pixels of a camera looking down at a floor half under a roof, the
// consistency grid's classes and reasons, and the grid of build 202's view on the Deck (fixtures/scene-221045-grid.bin). With an
// output folder it also writes the synthetic traced frame run.sh hands to trace_reference and check_trace.py.
#include "../core/alpha.h"
#include "../core/trace_files.h"
#include "../photoreal.h"

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

    bool near(float a, float b, float tolerance) { return std::fabs(a - b) <= tolerance; }
    bool near(const Vec3 &a, const Vec3 &b, float tolerance) { return near(a[0], b[0], tolerance) && near(a[1], b[1], tolerance) && near(a[2], b[2], tolerance); }
    float dot(const Vec3 &a, const Vec3 &b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

    void set_unity(float out[16], const double rows[16])
    {
        for (int r = 0; r < 4; r++)
            for (int c = 0; c < 4; c++)
                out[c * 4 + r] = float(rows[r * 4 + c]);
    }

    // A camera 2 m above the world origin looking straight down, its up toward +z, with a 90-degree reversed infinite
    // projection: clip z is 0.25 and w the distance, so the floor at y = 0 has depth 0.125 at every pixel.
    Camera looking_down()
    {
        Camera c {};
        const double view[16] = { 1, 0, 0, 0, 0, 0, 1, 0, 0, 1, 0, -2, 0, 0, 0, 1 };
        const double projection[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0.25, 0, 0, -1, 0 };
        set_unity(c.world_to_view, view);
        set_unity(c.view_to_clip, projection);
        return c;
    }

    TraceTriangle triangle(Vec3f a, Vec3f b, Vec3f c)
    {
        TraceTriangle t;
        t.corner = { a, b, c };
        return t;
    }

    // R10G10B10A2_UNORM as the G-buffer stores a world normal.
    uint32_t encode_normal(const Vec3 &n)
    {
        uint32_t v = 0;
        for (int k = 0; k < 3; k++)
            v |= static_cast<uint32_t>(std::lround((n[k] * 0.5f + 0.5f) * 1023)) << (k * 10);
        return v;
    }

    // The synthetic traced frame: a courtyard of a floor and three walls 4 m high with a 1 m cube in it and a 2 m foliage
    // card alpha-tested by the census shader 68b05754 with a checkerboard, 100 m east and 50 m south of the world's origin,
    // seen from inside at 96x64 with the sun low over the open side. Its depth, normals and stencil are ray cast exactly,
    // with the card's holes, its history is this kernel's own over 6 passes, and a broken copy has a block of sun samples
    // flipped, which check_trace.py must refuse.
    namespace synthetic
    {
        constexpr uint32_t kWidth = 96, kHeight = 64;

        void quad(std::vector<TraceTriangle> &out, Vec3f a, Vec3f b, Vec3f c, Vec3f d)
        {
            out.push_back(triangle(a, b, c));
            out.push_back(triangle(a, c, d));
        }

        Vec3 cross3(const Vec3 &a, const Vec3 &b) { return { a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0] }; }
        Vec3 unit3(const Vec3 &a)
        {
            const float l = std::sqrt(dot(a, a));
            return { a[0] / l, a[1] / l, a[2] / l };
        }

        struct Written
        {
            uint32_t shadowed = 0;  // pixels in full sun shadow
            uint32_t card = 0;      // pixels that show the card
            uint32_t holes = 0;     // pixels whose ray passes through a hole of the card, which it would meet whole
        };

        // Writes scene-trace-synthetic/trace-scene.bin, trace-synthetic/ and trace-synthetic-broken/ into out.
        Written write(const std::filesystem::path &out)
        {
            const Vec3d reference = { 100, 0, -50 };
            std::vector<TraceTriangle> triangles;
            quad(triangles, { -5, 0, -5 }, { 5, 0, -5 }, { 5, 0, 5 }, { -5, 0, 5 });
            quad(triangles, { -5, 0, -5 }, { -5, 0, 5 }, { -5, 4, 5 }, { -5, 4, -5 });
            quad(triangles, { 5, 0, -5 }, { 5, 4, -5 }, { 5, 4, 5 }, { 5, 0, 5 });
            quad(triangles, { -5, 0, -5 }, { -5, 4, -5 }, { 5, 4, -5 }, { 5, 0, -5 });
            const float x0 = 0.5f, x1 = 1.5f, z0 = -1.5f, z1 = -0.5f;
            quad(triangles, { x0, 0, z0 }, { x1, 0, z0 }, { x1, 1, z0 }, { x0, 1, z0 });
            quad(triangles, { x0, 0, z1 }, { x0, 1, z1 }, { x1, 1, z1 }, { x1, 0, z1 });
            quad(triangles, { x0, 0, z0 }, { x0, 1, z0 }, { x0, 1, z1 }, { x0, 0, z1 });
            quad(triangles, { x1, 0, z0 }, { x1, 0, z1 }, { x1, 1, z1 }, { x1, 1, z0 });
            quad(triangles, { x0, 1, z0 }, { x1, 1, z0 }, { x1, 1, z1 }, { x0, 1, z1 });
            // The card: t0.w of a 4x4 checkerboard against cb2[14].y = 0.5, its UV from its corners.
            std::ifstream file("fixtures/68b05754fdce207c.ps.dxbc", std::ios::binary);
            const std::vector<uint8_t> code((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            const AlphaAnalysis cutoff = analyze_alpha(code.data(), code.size());
            std::vector<uint8_t> atlas(kAtlasTile * kAtlasTile);
            for (uint32_t i = 0; i < atlas.size(); i++)
                atlas[i] = ((i % kAtlasTile / 64 + i / kAtlasTile / 64) % 2) ? 255 : 0;
            AlphaBuffer buffer;
            const float half = 0.5f;
            uint32_t half_bits;
            std::memcpy(&half_bits, &half, 4);
            const uint32_t draw = buffer.add_draw(buffer.add_program(cutoff.program), 0, 3 | 3 << 3, { 0, half_bits, 0, 0 });
            const Vec3f a = { -3, 0.2f, -1.5f }, b = { -1, 0.2f, -1.5f }, c = { -1, 2.2f, -1.5f }, d = { -3, 2.2f, -1.5f };
            const float uv_abc[6] = { 0, 0, 1, 0, 1, 1 }, uv_acd[6] = { 0, 0, 1, 1, 0, 1 };
            triangles.push_back(triangle(a, b, c));
            triangles.back().info = triangle_info(TraceClass::foliage, buffer.add_record(draw, uv_abc, 2));
            triangles.push_back(triangle(a, c, d));
            triangles.back().info = triangle_info(TraceClass::foliage, buffer.add_record(draw, uv_acd, 2));
            const std::vector<uint32_t> alpha = buffer.words(atlas);
            const std::filesystem::path scene = out / "scene-trace-synthetic";
            std::filesystem::create_directories(scene);
            const std::string bytes = trace_scene_bytes(triangles, reference, alpha);
            std::ofstream(scene / "trace-scene.bin", std::ios::binary).write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
            const Bvh bvh = build_bvh(triangles);
            Written written;

            // 1.6 m up and 4 m from the open side, looking toward -z and 15 degrees down, a 60-degree view.
            const Vec3 eye = { 0, 1.6f, 4 };
            const Vec3 forward = unit3({ 0, -std::sin(0.2618f), -std::cos(0.2618f) });
            const Vec3 right = unit3(cross3({ 0, 1, 0 }, forward)), up = cross3(forward, right);
            const double fy = 1 / std::tan(0.5236), fx = fy * kHeight / kWidth;
            double view[16] = {}, projection[16] = { fx, 0, 0, 0, 0, fy, 0, 0, 0, 0, 0, 0.1, 0, 0, -1, 0 };
            const Vec3 rows[3] = { right, up, { -forward[0], -forward[1], -forward[2] } };
            for (int r = 0; r < 3; r++)
            {
                const Vec3d world_eye = { reference[0] + eye[0], reference[1] + eye[1], reference[2] + eye[2] };
                for (int k = 0; k < 3; k++)
                    view[r * 4 + k] = rows[r][k];
                view[r * 4 + 3] = -(rows[r][0] * world_eye[0] + rows[r][1] * world_eye[1] + rows[r][2] * world_eye[2]);
            }
            view[15] = 1;
            Camera camera {};
            set_unity(camera.world_to_view, view);
            set_unity(camera.view_to_clip, projection);

            TraceImage image;
            image.size = { kWidth, kHeight };
            image.depth.assign(kWidth * kHeight, 0);
            image.normals.assign(kWidth * kHeight, 0);
            image.stencil.assign(kWidth * kHeight, 0);
            for (uint32_t y = 0; y < kHeight; y++)
                for (uint32_t x = 0; x < kWidth; x++)
                {
                    const float vx = float(((x + 0.5) / kWidth * 2 - 1) / fx), vy = float((1 - (y + 0.5) / kHeight * 2) / fy);
                    const Vec3 d = { right[0] * vx + up[0] * vy + forward[0], right[1] * vx + up[1] * vy + forward[1], right[2] * vx + up[2] * vy + forward[2] };
                    const std::optional<Hit> hit = trace_nearest(bvh, eye, d, 1e5f, kAllClasses | kAlphaTests, nullptr, &alpha);
                    const std::optional<Hit> whole = trace_nearest(bvh, eye, d, 1e5f);
                    if (!hit)
                        continue;
                    const BvhTriangle &t = bvh.triangles[hit->triangle];
                    const bool card = info_class(t.info) == TraceClass::foliage;
                    written.card += card;
                    written.holes += whole && info_class(bvh.triangles[whole->triangle].info) == TraceClass::foliage && !card;
                    Vec3 n = unit3(cross3({ t.e1[0], t.e1[1], t.e1[2] }, { t.e2[0], t.e2[1], t.e2[2] }));
                    if (dot(n, d) > 0)
                        n = { -n[0], -n[1], -n[2] };
                    const size_t at = size_t(y) * kWidth + x;
                    image.depth[at] = 0.1f / hit->t;
                    image.normals[at] = encode_normal(n);
                    image.stencil[at] = card ? 137 : 128;
                }

            TraceSettings settings;
            settings.ao_samples = 6;
            settings.sun_samples = 4;
            const Vec3 sun = unit3({ 0.5f, 0.6f, 0.3f });
            TraceSave save;
            save.size = image.size;
            save.constants = *trace_constants(settings, 0.6f, camera, false, image.size, reference, sun, 1);
            save.history.assign(size_t(kWidth) * kHeight * 4, 0);
            for (int pass = 0; pass < 6; pass++)
                for (uint32_t y = 0; y < kHeight; y++)
                    for (uint32_t x = 0; x < kWidth; x++)
                    {
                        float *h = &save.history[(size_t(y) * kWidth + x) * 4];
                        const PixelTrace r = trace_pixel(bvh, image, save.constants, x, y, { h[0], h[1], h[2], h[3] }, &alpha);
                        std::copy(r.history.begin(), r.history.end(), h);
                    }
            save.depth = image.depth;
            save.normals = image.normals;
            save.stencil = image.stencil;
            save.settings = settings;
            save.level = 0.6f;
            save.camera = camera;
            save.reference = reference;
            save.sun_source = "pushed";
            save.scene = "scene-trace-synthetic";
            save.generation = 1;
            save.bvh_hash = bvh.hash;
            save.triangles = static_cast<uint32_t>(bvh.triangles.size());
            save.nodes = static_cast<uint32_t>(bvh.nodes.size());
            save.passes = 6;
            write_trace_folder(out / "trace-synthetic", save);
            for (size_t i = 0; i < size_t(kWidth) * kHeight; i++)
                written.shadowed += save.history[i * 4 + 3] > 0 && save.history[i * 4 + 1] == 0;
            // A 24x24 block of the floor sees the sun where it did not and not where it did.
            for (uint32_t y = 36; y < 60; y++)
                for (uint32_t x = 30; x < 54; x++)
                {
                    float *h = &save.history[(size_t(y) * kWidth + x) * 4];
                    h[1] = h[3] - h[1];
                }
            write_trace_folder(out / "trace-synthetic-broken", save);
            return written;
        }
    }

    std::string slurp(const std::string &path)
    {
        std::ifstream file(path, std::ios::binary);
        std::string bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        if (bytes.empty())
            throw std::runtime_error("missing " + path);
        return bytes;
    }

    SunCbuffers load_sun(const std::string &path)
    {
        std::ifstream file(path, std::ios::binary);
        const std::string bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        SunCbuffers out {};
        if (bytes.size() != out.size())
            throw std::runtime_error("missing " + path);
        std::memcpy(out.data(), bytes.data(), out.size());
        return out;
    }
}

int main(int argc, char **argv)
{
    {
        std::printf("INFO seeds %08x %08x %08x %08x\n", pixel_seed(0, 0, 1), pixel_seed(1, 0, 1), pixel_seed(0, 1, 1), pixel_seed(5, 7, 2));
        const uint32_t seed = pixel_seed(5, 7, 2);
        for (uint32_t i = 0; i < 4; i++)
        {
            const std::array<float, 2> a = sequence_point(seed, 0, i), s = sequence_point(seed, 1, i);
            const Vec3 d = cosine_direction({ 0, 1, 0 }, a);
            std::printf("INFO index %u: occlusion point (%.9g, %.9g) direction (%.6f, %.6f, %.6f), sun point (%.9g, %.9g)\n", i, a[0], a[1], d[0], d[1], d[2], s[0], s[1]);
        }
        check("the seeds of four pixels are their literal hashes", pixel_seed(0, 0, 1) == 0x24009c6du && pixel_seed(1, 0, 1) == 0x6a25388au
            && pixel_seed(0, 1, 1) == 0x2fb3bc6fu && pixel_seed(5, 7, 2) == 0x81daa9f2u);
        check("pixel (5, 7)'s points and directions around +Y for indexes 0 and 3 are literal", sequence_point(seed, 0, 0) == std::array<float, 2> { 0.965142787f, 0.828011632f }
            && sequence_point(seed, 1, 3) == std::array<float, 2> { 0.587356865f, 0.264259636f }
            && near(cosine_direction({ 0, 1, 0 }, sequence_point(seed, 0, 0)), { 0.462491f, 0.186701f, 0.866744f }, 1e-6f)
            && near(cosine_direction({ 0, 1, 0 }, sequence_point(seed, 0, 3)), { -0.466082f, 0.877624f, 0.111997f }, 1e-6f));
        const std::array<float, 2> p0 = sequence_point(seed, 0, 0), p1 = sequence_point(seed, 0, 1);
        check("successive points step by the plastic number's R2 lattice, 0.7548777 and 0.5698403 a step, wrapping",
            near(std::fmod(p1[0] - p0[0] + 1.0f, 1.0f), 0.7548777f, 1e-6f) && near(std::fmod(p1[1] - p0[1] + 1.0f, 1.0f), 0.5698403f, 1e-6f));
        check("the occlusion and sun sequences of a pixel differ", sequence_point(seed, 0, 0) != sequence_point(seed, 1, 0));
    }
    {
        // The means over 1e5 points: cosine-weighted directions have a mean cosine of 2/3, and their sideways parts
        // cancel; a cone's mean cosine is (1 + cos_max) / 2 and every direction lies inside it.
        const float s = 1 / std::sqrt(14.0f);
        const Vec3 normal = { 1 * s, 2 * s, 3 * s }, side = { 2 / std::sqrt(5.0f), -1 / std::sqrt(5.0f), 0 };
        const float cos_max = std::cos(0.2f);
        double cosine = 0, sideways = 0, cone = 0, unit = 0;
        bool inside = true;
        const uint32_t seed = pixel_seed(3, 4, 5);
        for (uint32_t i = 0; i < 100000; i++)
        {
            const Vec3 d = cosine_direction(normal, sequence_point(seed, 0, i));
            cosine += dot(d, normal);
            sideways += dot(d, side);
            unit = std::max<double>(unit, std::fabs(dot(d, d) - 1));
            const Vec3 c = cone_direction(normal, cos_max, sequence_point(seed, 1, i));
            cone += dot(c, normal);
            inside &= dot(c, normal) >= cos_max - 1e-6f;
        }
        check("cosine-weighted directions: mean cosine 2/3 and no mean sideways, all unit length", near(float(cosine / 1e5), 2.0f / 3, 2e-3f)
            && near(float(sideways / 1e5), 0, 2e-3f) && unit < 1e-5);
        check("cone directions: mean cosine (1 + cos 0.2) / 2, all inside", near(float(cone / 1e5), (1 + cos_max) / 2, 1e-4f) && inside);
        check("a cone of size 0 is its axis", near(cone_direction(normal, 1, sequence_point(seed, 1, 7)), normal, 1e-6f));
    }
    {
        const std::vector<uint32_t> order = tile_order({ 200, 130 });
        const std::vector<uint32_t> want = { 0, 1, 1u << 16, 1 | 1u << 16, 2, 3, 2 | 1u << 16, 3 | 1u << 16, 2u << 16, 1 | 2u << 16, 2 | 2u << 16, 3 | 2u << 16 };
        check("200x130 has 4x3 tiles of 64, in Morton order", order == want);
        check("1920x1200 has 30x19 tiles", tile_order({ 1920, 1200 }).size() == 570);
    }
    {
        TraceSchedule s;
        s.tiles = 5;
        s.batch = 2;
        std::string seen;
        while (const std::optional<TraceDispatch> d = trace_next(s, 2))
            seen += std::to_string(d->first) + "+" + std::to_string(d->count) + "/" + std::to_string(trace_passes(s)) + " ";
        check("5 tiles, 2 a frame, 2 samples: the cursor wraps, never traces a tile twice in a frame, and stops after 2 passes",
            seen == "0+2/0 2+2/0 4+2/1 1+2/1 3+2/2 " && !trace_next(s, 2) && trace_next(s, 3));
        TraceSchedule big;
        big.tiles = 3;
        big.batch = 50;
        check("a batch past every tile traces each once", trace_next(big, 4)->count == 3);
        check("the budget: 4 tiles in 10 ms of 20 doubles, 8 in 40 of 20 halves, at most twice the batch and at most every tile, "
              "at least 1, and twice with no measure", next_batch(4, 4, 10, 20, 100) == 8 && next_batch(8, 8, 40, 20, 100) == 4
            && next_batch(4, 4, 1, 20, 100) == 8 && next_batch(60, 60, 10, 20, 100) == 100 && next_batch(1, 1, 100, 20, 50) == 1
            && next_batch(4, 4, 0, 20, 100) == 8 && next_batch(6, 6, 15, 20, 100) == 8);
        check("the budget from the first part only: a frame of 100 tiles whose first 10 took 1 ms wants 200 but grows at most to twice "
              "the frame, and 10 tiles in 5 ms of a 20 ms budget give 40", next_batch(100, 10, 1, 20, 1000) == 200
            && next_batch(100, 10, 5, 20, 1000) == 40);
        TraceSchedule first;
        first.tiles = 570;
        const std::optional<TraceDispatch> one = trace_next(first, 64);
        check("a new scene's first frame traces 1 tile in a submission of 1, so its measured time sets the next frames' before any "
              "frame traces more", one && one->first == 0 && one->count == 1 && first.chunk == 1 && next_batch(first.batch, 1, 0.5, 20, 570) == 2);
        check("a submission holds 50 ms of tiles: 1 tile of 0.02 ms gives 64 at most, 1 of 5 ms gives 10, 1 of 80 ms or no measure 1",
            next_chunk(1, 0.02) == kMaxChunkTiles && kMaxChunkTiles == 64 && next_chunk(1, 5) == 10 && next_chunk(2, 10) == 10
            && next_chunk(1, 80) == 1 && next_chunk(1, 0) == 1);
        const auto chunks = [](TraceDispatch d, uint32_t tiles, uint32_t chunk) {
            std::string out;
            for (const TraceDispatch &c : trace_chunks(d, tiles, chunk))
                out += std::to_string(c.first) + "+" + std::to_string(c.count) + " ";
            return out;
        };
        check("a frame's tiles go to the GPU a chunk at a time, wrapping: 7 from 566 of 570 by 2 as 566+2 568+2 0+2 2+1, by 64 as "
              "566+7, 1 as 1, none as none, and a chunk of 0 as 1",
            chunks({ 566, 7 }, 570, 2) == "566+2 568+2 0+2 2+1 " && chunks({ 566, 7 }, 570, 64) == "566+7 " && chunks({ 3, 1 }, 570, 2) == "3+1 "
            && chunks({ 0, 0 }, 570, 2).empty() && chunks({ 5, 2 }, 570, 0) == "5+1 6+1 ");
    }
    {
        const Camera camera = looking_down();
        TraceSettings settings;
        const TraceKey a = trace_key(1, { 64, 64 }, camera, { 0, 1, 0 }, settings);
        Camera nudged = camera;
        nudged.world_to_view[13] += 1e-7f;
        Camera moved = camera;
        moved.world_to_view[13] += 1e-4f;
        TraceSettings other = settings;
        other.characters = false;
        TraceSettings more = settings;
        more.ao_samples = 128;
        check("the samples start again for a moved camera, sun, scene, render size or casters, not for noise or more samples",
            !trace_reset(a, trace_key(1, { 64, 64 }, nudged, { 0, 1, 0 }, settings)) && trace_reset(a, trace_key(1, { 64, 64 }, moved, { 0, 1, 0 }, settings))
            && trace_reset(a, trace_key(1, { 64, 64 }, camera, { 0, 1, 2e-5f }, settings)) && trace_reset(a, trace_key(2, { 64, 64 }, camera, { 0, 1, 0 }, settings))
            && trace_reset(a, trace_key(1, { 64, 32 }, camera, { 0, 1, 0 }, settings)) && trace_reset(a, trace_key(1, { 64, 64 }, camera, { 0, 1, 0 }, other))
            && !trace_reset(a, trace_key(1, { 64, 64 }, camera, { 0, 1, 0 }, more)));
        check("the classes a ray sees, with the alpha tests", caster_classes(settings) == (0x1F | kAlphaTests) && caster_classes(other) == (0x0F | kAlphaTests));
        other.alpha_cards = false;
        check("without cards and characters only the world and other", caster_classes(other) == (0x09 | kAlphaTests));
        TraceSettings whole = settings;
        whole.whole_cards = true;
        check("whole cards: the same classes without the alpha tests, and a change of it starts the samples again", caster_classes(whole) == 0x1F
            && trace_reset(a, trace_key(1, { 64, 64 }, camera, { 0, 1, 0 }, whole)));
    }
    {
        const SunFacts facts = sun_facts(load_sun("fixtures/scene-194351-face-6-sun.bin"));
        check("build 199's view face: the sun from cascade 0's depth gradient and the floating origin from b1 row 10",
            facts.toward_sun && near(*facts.toward_sun, { 0.246589f, 0.573576f, 0.781155f }, 1e-6f) && facts.floating_origin
            && near(*facts.floating_origin, { 1648.686f, 0, 9468.529f }, 1e-3f));
        SunCbuffers broken {};
        const float nan = NAN;
        std::memcpy(broken.data() + (48 + 10) * 16, &nan, 4);
        check("zero cascades give no sun, and a NaN origin no origin", !sun_facts(broken).toward_sun && !sun_facts(broken).floating_origin);
        check("the origin moves past 1 cm", !origin_moved({ 1, 2, 3 }, { 1.005f, 2, 3 }) && origin_moved({ 1, 2, 3 }, { 1.02f, 2, 3 }));
        check("the scene is stale past 5% of at least 32 rays off", !consistency_stale(31, 31) && !consistency_stale(100, 5) && consistency_stale(100, 6));
    }
    {
        const Camera camera = looking_down();
        TraceSettings settings;
        const std::optional<TraceConstants> c = trace_constants(settings, 0.6f, camera, false, { 8, 8 }, { 0, 0.5, 0 }, { 0, 2, 0 }, 9);
        check("the constants: the eye relative to the reference, the sun's cone for 0.0093, the parameters and counts", c
            && near(c->eye_position[0], 0, 1e-6f) && near(c->eye_position[1], 1.5f, 1e-6f) && near(c->eye_position[2], 0, 1e-6f)
            && c->sun_cone[1] == 1.0f && near(c->sun_cone[3], 0.99998919f, 1e-7f) && c->trace_params[0] == 2.0f && c->trace_params[3] == 0.6f
            && c->trace_counts[0] == 64 && c->trace_counts[1] == 32 && c->trace_counts[2] == (0x1F | kAlphaTests) && c->trace_counts[3] == 3 && c->trace_tiles[3] == 9
            && c->trace_render[0] == 8 && c->trace_render[2] == 0 && c->trace_render[3] == 16);
        // Clip (0.5, -0.5, 0.125, 1) is 2 m below the eye, half a distance right and half back: (1, 0, -1) in the world.
        const float clip[4] = { 0.5f, -0.5f, 0.125f, 1 };
        float h[4] = {};
        for (int r = 0; r < 4; r++)
            for (int k = 0; k < 4; k++)
                h[r] += c->clip_rows[r][k] * clip[k];
        check("clip_rows take clip space to the scene's frame", near(h[0] / h[3], 1, 1e-5f) && near(h[1] / h[3], -0.5f, 1e-5f) && near(h[2] / h[3], -1, 1e-5f));
        check("a projection that does not invert gives no constants", !trace_constants(settings, 1, Camera {}, false, { 8, 8 }, { 0, 0, 0 }, { 0, 1, 0 }, 0));
        const Vec3d at = camera_position(camera);
        check("the camera's position from its view", at[0] == 0 && at[1] == 2 && at[2] == 0);
    }
    {
        // An 8x8 frame of the floor below the camera, world stencil and up normals, and a scene of the floor and a roof 1 m
        // above it over x > 0. The sun is straight up and the reference the world origin.
        TraceImage image;
        image.size = { 8, 8 };
        image.depth.assign(64, 0.125f);
        image.normals.assign(64, encode_normal({ 0, 1, 0 }));
        image.stencil.assign(64, 128);
        image.stencil[1 * 8 + 1] = 133;
        image.depth[2 * 8 + 2] = 0;
        const std::vector<TraceTriangle> triangles = {
            triangle({ -10, 0, -10 }, { 10, 0, -10 }, { 10, 0, 10 }), triangle({ -10, 0, -10 }, { 10, 0, 10 }, { -10, 0, 10 }),
            triangle({ 0, 1, -10 }, { 10, 1, -10 }, { 10, 1, 10 }), triangle({ 0, 1, -10 }, { 10, 1, 10 }, { 0, 1, 10 }),
        };
        const Bvh bvh = build_bvh(triangles);
        TraceSettings settings;
        settings.sun_size = 0;
        TraceConstants c = *trace_constants(settings, 1, looking_down(), false, { 8, 8 }, { 0, 0, 0 }, { 0, 1, 0 }, 7);

        // The occlusion a pixel's index-th ray should find: the ray of the C++ generator from the pixel's floor point, offset
        // 0.02 m + 1e-4 of its distance up, against the roof's plane, 1 m up over 0 <= x <= 10.
        Vec3 n = { 0, 0, 0 };
        for (int k = 0; k < 3; k++)
            n[k] = (((encode_normal({ 0, 1, 0 }) >> (k * 10)) & 1023u) / 1023.0f) * 2 - 1;
        const float length = std::sqrt(dot(n, n));
        n = { n[0] / length, n[1] / length, n[2] / length };
        const auto expected = [&](uint32_t px, uint32_t py, uint32_t index) {
            const float x = ((px + 0.5f) / 8 * 2 - 1) * 2, z = (1 - (py + 0.5f) / 8 * 2) * 2;
            const float lift = 0.02f + 1e-4f * std::sqrt(x * x + 4 + z * z);
            const Vec3 d = cosine_direction(n, sequence_point(pixel_seed(px, py, 7), 0, index));
            const float t = (1 - lift) / d[1], hit_x = x + t * d[0], hit_z = z + t * d[2];
            return t < 2 && hit_x >= 0 && hit_x <= 10 && std::fabs(hit_z) <= 10 ? (t / 2) * (t / 2) : 1.0f;
        };
        // Pixel (6, 3) lies at (1.25, 0, 0.25), under the roof: eight dispatches.
        PixelTrace under;
        float under_want = 0;
        for (uint32_t i = 0; i < 8; i++)
        {
            under = trace_pixel(bvh, image, c, 6, 3, under.history);
            under_want += expected(6, 3, i);
        }
        std::printf("INFO pixel (6, 3) after 8 dispatches: occlusion sum %.6f, expected %.6f; sun %.0f\n", under.history[0], under_want, under.history[1]);
        check("under the roof: (t / 2)^2 for each occlusion ray's hit within 2 m, 1 for a miss, and the sun blocked", under.world
            && near(under.history[0], under_want, 1e-3f) && under_want < 7 && under.history[2] == 8 && under.history[1] == 0 && under.history[3] == 8
            && under.counters[0] == 2);
        const PixelTrace open = trace_pixel(bvh, image, c, 1, 3, { 0, 0, 0, 0 });
        check("beside the roof: the occlusion its ray should find, and the sun", near(open.history[0], expected(1, 3, 0), 2e-4f) && open.history[1] == 1
            && open.history[2] == 1 && open.history[3] == 1);
        const PixelTrace again = trace_pixel(bvh, image, c, 1, 3, open.history);
        check("a second dispatch adds the next sample of the sequence", near(again.history[0], open.history[0] + expected(1, 3, 1), 2e-4f)
            && again.history[1] == 2 && again.history[2] == 2 && again.history[3] == 2);
        const PixelTrace grid = trace_pixel(bvh, image, c, 0, 0, { 0, 0, 0, 0 });
        const auto grid_counts = [](const PixelTrace &p) {
            return std::vector<uint32_t>(p.counters.begin() + static_cast<long>(TraceCounter::grid), p.counters.end());
        };
        check("the grid's pixel shoots a ray from the camera that meets the floor where the depth says", grid.counters[3] == 1 && grid.counters[4] == 0
            && grid.counters[0] == 3 && grid_counts(grid) == std::vector<uint32_t> { 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 });
        const Bvh roof_only = build_bvh({ triangles[2], triangles[3] });
        const PixelTrace missing = trace_pixel(roof_only, image, c, 0, 0, { 0, 0, 0, 0 });
        check("without the floor that ray finds nothing, and counts as off, of stencil 128, for meeting nothing", missing.counters[3] == 1
            && missing.counters[4] == 1 && grid_counts(missing) == std::vector<uint32_t> { 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 });
        // The floor 0.5 m lower and higher, under a stencil-144 pixel and another world stencil's.
        const Bvh lower = build_bvh({ triangle({ -10, -0.5f, -10 }, { 10, -0.5f, -10 }, { 10, -0.5f, 10 }), triangle({ -10, -0.5f, -10 }, { 10, -0.5f, 10 }, { -10, -0.5f, 10 }) });
        const Bvh higher = build_bvh({ triangle({ -10, 0.5f, -10 }, { 10, 0.5f, -10 }, { 10, 0.5f, 10 }), triangle({ -10, 0.5f, -10 }, { 10, 0.5f, 10 }, { -10, 0.5f, 10 }) });
        TraceImage terrain = image;
        terrain.stencil[0] = 144;
        const PixelTrace farther = trace_pixel(lower, terrain, c, 0, 0, { 0, 0, 0, 0 });
        terrain.stencil[0] = 160;
        const PixelTrace nearer = trace_pixel(higher, terrain, c, 0, 0, { 0, 0, 0, 0 });
        check("a floor 0.5 m lower is off farther, counted for stencil 144; one 0.5 m higher off nearer, for the other world stencils",
            farther.counters[4] == 1 && grid_counts(farther) == std::vector<uint32_t> { 0, 0, 0, 0, 1, 0, 0, 1, 0, 0, 0, 0 } && nearer.counters[4] == 1
            && grid_counts(nearer) == std::vector<uint32_t> { 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 1, 0 });
        TraceCounts counts {};
        for (const PixelTrace *p : { &grid, &missing, &farther, &nearer })
            for (uint32_t k = 0; k < kTraceCounters; k++)
                counts[k] += p->counters[k];
        check("the grid's breakdown for the log names each class's rays off of tested, and the reasons where some are off",
            grid_breakdown(counts) == "stencil 128 1 of 2 off (1 met nothing, 0 nearer, 0 farther), stencil 144 1 of 1 off (0 met nothing, 0 nearer, 1 farther), "
                                      "other 1 of 1 off (0 met nothing, 1 nearer, 0 farther)");
        check("a character's pixel and a pixel without depth are not traced", !trace_pixel(bvh, image, c, 1, 1, {}).world
            && !trace_pixel(bvh, image, c, 2, 2, {}).world);
        const PixelTrace done = trace_pixel(bvh, image, c, 1, 3, { 64, 32, 64, 32 });
        check("a pixel with all its samples traces nothing", done.history == std::array<float, 4> { 64, 32, 64, 32 } && done.counters[0] == 0);
        c.sun_cone[0] = c.sun_cone[1] = c.sun_cone[2] = c.sun_cone[3] = 0;
        const PixelTrace sunless = trace_pixel(bvh, image, c, 6, 3, { 0, 0, 0, 0 });
        check("without a sun its samples count as lit, untraced", sunless.history[1] == 1 && sunless.history[3] == 1 && sunless.counters[0] == 1);
    }
    {
        // Build 202 on the Deck: the consistency grid of the view-only capture scene-20261011-221045 at the spot of the
        // self-test's save trace-20261011-221018, the grid's pixels within 15 m and the world and other triangles within
        // 25 m (fixtures/scene-221045-grid.bin, from make_grid_fixture.py). The pushed camera, the scene's reference, the
        // stored rows' flip and the depth's decoding put every stencil-128 pixel's ray on the depth buffer, while every
        // stencil-144 pixel met the scene farther: the terrain near the camera, drawn tessellated, was left out of the
        // capture, and the rays met the prepass's untessellated terrain under it. That scene went stale after pass 1.
        const std::string bytes = slurp("fixtures/scene-221045-grid.bin");
        uint32_t pixels = 0, count = 0;
        std::memcpy(&pixels, bytes.data() + 4, 4);
        std::memcpy(&count, bytes.data() + 8, 4);
        Camera camera {};
        Vec3d reference {};
        std::memcpy(camera.world_to_view, bytes.data() + 12, 64);
        std::memcpy(camera.view_to_clip, bytes.data() + 76, 64);
        std::memcpy(reference.data(), bytes.data() + 140, 24);
        const size_t samples_at = 164, triangles_at = samples_at + size_t(pixels) * 12;
        std::vector<TraceTriangle> triangles(count);
        for (uint32_t i = 0; i < count; i++)
        {
            std::memcpy(triangles[i].corner.data(), bytes.data() + triangles_at + size_t(i) * 40, 36);
            std::memcpy(&triangles[i].info, bytes.data() + triangles_at + size_t(i) * 40 + 36, 4);
        }
        const Size size = { 1920, 1200 };
        TraceImage image;
        image.size = size;
        image.depth.assign(size_t(size.width) * size.height, 0.0f);
        image.normals.assign(image.depth.size(), 0);
        image.stencil.assign(image.depth.size(), 0);
        std::vector<std::array<uint32_t, 2>> grid;
        for (uint32_t i = 0; i < pixels; i++)
        {
            const char *at = bytes.data() + samples_at + size_t(i) * 12;
            uint16_t x = 0, y = 0;
            std::memcpy(&x, at, 2);
            std::memcpy(&y, at + 2, 2);
            const size_t texel = size_t(y) * size.width + x;
            std::memcpy(&image.depth[texel], at + 4, 4);
            image.stencil[texel] = static_cast<uint8_t>(at[8]);
            grid.push_back({ x, y });
        }
        const Bvh bvh = build_bvh(triangles);
        TraceSettings settings;
        const std::optional<TraceConstants> c = trace_constants(settings, 1, camera, true, size, reference, { 0.239f, 0.574f, -0.784f }, 2);
        TraceCounts counts {};
        for (const auto &[x, y] : grid)
        {
            // Every sample taken, so only the grid's ray is shot.
            const PixelTrace p = trace_pixel(bvh, image, *c, x, y, { 0, 0, 1e9f, 1e9f });
            for (uint32_t k = 0; k < kTraceCounters; k++)
                counts[k] += p.counters[k];
        }
        std::printf("INFO build 202's grid within 15 m: %s\n", grid_breakdown(counts).c_str());
        check("build 202's view at the Deck's spot: all 1307 stencil-128 grid pixels within 15 m meet the scene where the depth says",
            counts[grid_counter(GridClass::s128, GridSlot::tested)] == 1307 && counts[grid_counter(GridClass::s128, GridSlot::no_hit)] == 0
                && counts[grid_counter(GridClass::s128, GridSlot::nearer)] == 0 && counts[grid_counter(GridClass::s128, GridSlot::farther)] == 0);
        check("build 202's stencil-144 grid pixels within 15 m: 1920 of 2105 meet the scene farther, none nearer, none nothing, so the scene is stale",
            counts[grid_counter(GridClass::s144, GridSlot::tested)] == 2105 && counts[grid_counter(GridClass::s144, GridSlot::farther)] == 1920
                && counts[grid_counter(GridClass::s144, GridSlot::nearer)] == 0 && counts[grid_counter(GridClass::s144, GridSlot::no_hit)] == 0
                && consistency_stale(counts[static_cast<size_t>(TraceCounter::tested)], counts[static_cast<size_t>(TraceCounter::off)]));
    }
    if (argc > 1)
    {
        const synthetic::Written written = synthetic::write(argv[1]);
        std::printf("INFO the synthetic traced frame has %u pixels in full sun shadow, %u of the card and %u through its holes\n", written.shadowed,
            written.card, written.holes);
        check("the synthetic traced frame is written, with sun shadows, the card and the wall through its holes in it",
            std::filesystem::file_size(std::filesystem::path(argv[1]) / "trace-synthetic" / "history.f32") == synthetic::kWidth * synthetic::kHeight * 16
                && std::filesystem::exists(std::filesystem::path(argv[1]) / "scene-trace-synthetic" / "trace-scene.bin") && written.shadowed > 100
                && written.card > 50 && written.holes > 50);
    }
    std::printf("%s: %d failed\n", __FILE__, failures);
    return failures == 0 ? 0 : 1;
}
