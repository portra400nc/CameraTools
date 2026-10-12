// The alpha analysis of census pixel shaders (tests/fixtures/<hash>.ps.dxbc): each discard's kind, the program's inputs,
// constant rows, texture and stream-output entries, as alpha_census.py found them; the interpreter (shaders/alpha.hlsli
// compiled as C++) on the leaf shader 0a48f0bb's program against the shader's own arithmetic worked by hand, on the
// per-instance cutoff of 5e9447eb with its flat instance and dynamically indexed row; and rays through an alpha-tested card.
#include "../core/alpha.h"
#include "../core/bvh.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <random>
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

    AlphaAnalysis analyze(const std::string &name)
    {
        std::ifstream file("fixtures/" + name, std::ios::binary);
        const std::vector<uint8_t> data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        if (data.empty())
            throw std::runtime_error("missing fixtures/" + name);
        return analyze_alpha(data.data(), data.size());
    }

    std::string kinds(const AlphaAnalysis &a)
    {
        std::string out;
        for (DiscardKind k : a.discards)
            out += (out.empty() ? "" : ",") + std::string(kDiscardKindNames[static_cast<size_t>(k)]);
        return out;
    }

    std::string rows(const AlphaProgram &p)
    {
        std::string out;
        for (const CbRows &r : p.rows)
            out += (out.empty() ? "" : " ") + ("cb" + std::to_string(r.slot)) + (r.dynamic ? "[dynamic]" : "[" + std::to_string(r.first) + "+" + std::to_string(r.count) + "]");
        return out;
    }

    uint32_t bits(float f)
    {
        uint32_t u;
        std::memcpy(&u, &f, 4);
        return u;
    }

    // A row of four floats, as a constant buffer holds it.
    std::vector<uint32_t> row(float x, float y, float z, float w) { return { bits(x), bits(y), bits(z), bits(w) }; }

    void append(std::vector<uint32_t> &to, const std::vector<uint32_t> &rows) { to.insert(to.end(), rows.begin(), rows.end()); }

    constexpr uint32_t kWrap = 1, kClamp = 3;
    uint32_t address(uint32_t mode) { return mode | mode << 3; }
}

int main()
{
    {
        const AlphaAnalysis leaves = analyze("0a48f0bb29e3c15d.ps.dxbc");
        check("0a48f0bb (leaves, 136 and 137): one texture discard kept as a program of 13 instructions", kinds(leaves) == "texture" && leaves.program
            && leaves.program->code.size() == 13);
        const AlphaProgram &p = *leaves.program;
        check("its inputs are TEXCOORD0.xy and the world position in TEXCOORD1.w, TEXCOORD2.w and TEXCOORD3.w, none flat",
            p.describe() == "TEXCOORD0.xy TEXCOORD1.w TEXCOORD2.w TEXCOORD3.w" && p.attribute_floats() == 5 && !p.inputs[0].flat && !p.inputs[3].flat);
        check("its rows are cb1[5], the camera, and cb3[11] to cb3[15], the distance boost and the cutoff; it samples t0.z with s0",
            rows(p) == "cb1[5+1] cb3[11+5]" && p.static_rows() == 6 && p.texture == 0 && p.channel == 2 && p.sampler == 0);
        const std::vector<SoEntry> so = p.so_entries();
        check("it streams TEXCOORD0 from component 0 for 2, and TEXCOORD1 to 3 from component 3 for 1 each",
            so.size() == 4 && so[0].semantic == "TEXCOORD" && so[0].index == 0 && so[0].start == 0 && so[0].count == 2 && so[1].index == 1
                && so[1].start == 3 && so[1].count == 1 && so[3].index == 3 && so[3].start == 3 && so[3].count == 1);

        const AlphaAnalysis dither = analyze("836da523e2ec9ecb.ps.dxbc");
        check("836da523: two screen-door dither discards are dropped, and the draw is opaque", kinds(dither) == "dither,dither" && !dither.program
            && dither.why.empty());
        const AlphaAnalysis fade = analyze("ee0c3fbbf7ebfae7.ps.dxbc");
        check("ee0c3fbb: two dither discards dropped, its texture cutoff t0.w against cb0[97] kept", kinds(fade) == "dither,dither,texture" && fade.program
            && fade.program->describe() == "TEXCOORD4.xy" && rows(*fade.program) == "cb0[97+1]" && fade.program->channel == 3);
        const AlphaAnalysis instanced = analyze("5e9447eb5599aa41.ps.dxbc");
        check("5e9447eb: the per-instance cutoff reads the instance flat and cb1 by it, the whole bound range",
            kinds(instanced) == "texture" && instanced.program && instanced.program->describe() == "TEXCOORD0.xy SV_InstanceID0.x"
                && instanced.program->inputs[1].flat && rows(*instanced.program) == "cb0[0+1] cb1[dynamic]");
        const AlphaAnalysis culled = analyze("a4cbd77dc355159d.ps.dxbc");
        check("a4cbd77d: the cull beyond 1 / cb1[6].w from the camera and the alpha test t1.w against cb3[5], in one program",
            kinds(culled) == "other,texture" && culled.program && culled.program->describe() == "TEXCOORD0.xy TEXCOORD6.xyz"
                && rows(*culled.program) == "cb1[5+2] cb3[5+1]" && culled.program->texture == 1 && culled.program->channel == 3);
        const AlphaAnalysis grass = analyze("aae32fcfa5c1a0bb.ps.dxbc");
        check("aae32fcf (grass): a computed UV, TEXCOORD3.xy scaled by cb0's rows, into t0.x", kinds(grass) == "texture" && grass.program
            && grass.program->describe() == "TEXCOORD3.xy" && grass.program->channel == 0);
        const AlphaAnalysis facing = analyze("0b0d6421c1054820.ps.dxbc");
        check("0b0d6421: the cutoff that also fades with |N.V| reads the interpolated normal too", kinds(facing) == "texture" && facing.program
            && facing.program->describe() == "TEXCOORD0.xy COLOR0.xyz TEXCOORD1.w TEXCOORD2.w TEXCOORD3.w" && facing.program->code.size() == 22);
        const AlphaAnalysis cutoff = analyze("68b05754fdce207c.ps.dxbc");
        check("68b05754 (world, 128): t0.w against cb2[14].y with sampler 3", kinds(cutoff) == "texture" && cutoff.program
            && cutoff.program->describe() == "TEXCOORD0.xy" && rows(*cutoff.program) == "cb2[14+1]" && cutoff.program->sampler == 3);
        const AlphaAnalysis character = analyze("41581d1e6aa3448f.ps.dxbc");
        check("41581d1e (a character): two dither and two texture discards on two textures, no program", kinds(character) == "dither,dither,texture,texture"
            && !character.program && character.why == "two textures or channels");
        const AlphaAnalysis prepass = analyze("482d98462fb30496.ps.dxbc");
        check("482d9846 (the depth prepass's leaves): the leaf test with the normal's fade, at cb0 and cb1", prepass.program
            && rows(*prepass.program) == "cb0[5+1] cb1[11+5]");
    }
    {
        // 0a48f0bb's discard by hand: keep where t0.z(uv) + saturate(|cb1[5] - pos| / (cb3[11].y + 1e-4)) * cb3[11].z - cb3[15].w
        // is not negative. Here cb3[11].y = 20, .z = 0.2 and cb3[15].w = 0.5, so near the camera the boost is about 0.1 and far
        // from it 0.2, and the tile's texel x holds x, so t0.z(u) is about u.
        const AlphaAnalysis leaves = analyze("0a48f0bb29e3c15d.ps.dxbc");
        std::vector<uint8_t> atlas(kAtlasTile * kAtlasTile);
        for (uint32_t i = 0; i < atlas.size(); i++)
            atlas[i] = static_cast<uint8_t>(i % kAtlasTile);
        const auto buffer = [&](float camera_x, uint32_t mode, float u1) {
            AlphaBuffer b;
            std::vector<uint32_t> constants;
            append(constants, row(camera_x, 0, 0, 0));
            append(constants, row(0, 20, 0.2f, 0));
            append(constants, row(0, 0, 0, 0));
            append(constants, row(0, 0, 0, 0));
            append(constants, row(0, 0, 0, 0));
            append(constants, row(0, 0, 0, 0.5f));
            const uint32_t draw = b.add_draw(b.add_program(leaves.program), 0, address(mode), constants);
            // Corners: (u, v, x, y, z) with the hit's barycentric (u, v) as its UV, all at the world's origin.
            const float corners[15] = { 0, 0, 0, 0, 0, u1, 0, 0, 0, 0, 0, 1, 0, 0, 0 };
            const uint32_t record = b.add_record(draw, corners, 5);
            return std::pair { b.words(atlas), record };
        };
        const auto [near, record] = buffer(10, kWrap, 1);
        check("near the camera, 0.2 of the way across, texel 50.7 (0.199) plus 0.1 is under 0.5: discarded", !alpha_keeps(near, record, 0.2f, 0.1f));
        check("0.6 of the way across, 0.600 plus 0.1: kept", alpha_keeps(near, record, 0.6f, 0.1f));
        check("0.35 across near the camera, 0.348 plus 0.1: discarded", !alpha_keeps(near, record, 0.35f, 0.3f));
        const auto [far, far_record] = buffer(1000, kWrap, 1);
        check("1 km from the camera the boost is 0.2, and 0.348 plus 0.2 is kept", alpha_keeps(far, far_record, 0.35f, 0.3f));
        const auto [wrapped, wrapped_record] = buffer(10, kWrap, 2);
        const auto [clamped, clamped_record] = buffer(10, kClamp, 2);
        check("a UV of 1.2 wraps to 0.2 and is discarded, and clamps to 1 and is kept", !alpha_keeps(wrapped, wrapped_record, 0.6f, 0.1f)
            && alpha_keeps(clamped, clamped_record, 0.6f, 0.1f));
    }
    {
        // 5e9447eb: keep where t0.w(uv) - cb1[instance + cb0[0].x].x is not negative. cb0[0].x = 2, so instance 1 reads cb1[3].x
        // = 0.25 and instance 3 cb1[5].x = 0.75, against a tile of 128 (0.502). The second and third corners hold another
        // instance, which a flat input never reads.
        const AlphaAnalysis instanced = analyze("5e9447eb5599aa41.ps.dxbc");
        const std::vector<uint8_t> atlas(kAtlasTile * kAtlasTile, 128);
        AlphaBuffer b;
        std::vector<uint32_t> constants = { 2, 0, 0, 0 };
        for (uint32_t i = 0; i < 8; i++)
            append(constants, row(i == 3 ? 0.25f : i == 5 ? 0.75f : 0.0f, 0, 0, 0));
        const uint32_t draw = b.add_draw(b.add_program(instanced.program), 0, address(kWrap), constants);
        const auto corners = [](uint32_t instance) {
            std::array<float, 9> c {};
            const uint32_t other = 7;
            const float values[9] = { 0, 0, 0, 1, 0, 0, 0, 1, 0 };
            std::copy(values, values + 9, c.begin());
            std::memcpy(&c[2], &instance, 4);
            std::memcpy(&c[5], &other, 4);
            std::memcpy(&c[8], &other, 4);
            return c;
        };
        const uint32_t one = b.add_record(draw, corners(1).data(), 3), three = b.add_record(draw, corners(3).data(), 3),
                       past = b.add_record(draw, corners(100).data(), 3);
        const std::vector<uint32_t> words = b.words(atlas);
        check("instance 1 against cb1[3] = 0.25 is kept, instance 3 against cb1[5] = 0.75 discarded, whatever the other corners hold",
            alpha_keeps(words, one, 0.4f, 0.4f) && !alpha_keeps(words, three, 0.4f, 0.4f) && alpha_keeps(words, three, 0.0f, 0.0f) == false);
        check("a row past the draw's reads 0, and 0.502 against 0 is kept", alpha_keeps(words, past, 0.4f, 0.4f));
    }
    {
        // A 2 m card at z = 2 in front of a wall at z = 4, alpha-tested by 68b05754 (t0.w against cb2[14].y = 0.5) with a
        // checkerboard of 8 by 8 squares: a ray through a dark square meets the wall, one through a light square the card.
        const AlphaAnalysis cutoff = analyze("68b05754fdce207c.ps.dxbc");
        std::vector<uint8_t> atlas(kAtlasTile * kAtlasTile);
        for (uint32_t y = 0; y < kAtlasTile; y++)
            for (uint32_t x = 0; x < kAtlasTile; x++)
                atlas[y * kAtlasTile + x] = ((x / 32 + y / 32) % 2) ? 255 : 0;
        AlphaBuffer b;
        std::vector<uint32_t> constants;
        append(constants, row(0, 0.5f, 0, 0));
        const uint32_t draw = b.add_draw(b.add_program(cutoff.program), 0, address(kClamp), constants);
        std::vector<TraceTriangle> triangles(4);
        triangles[0].corner = { Vec3f { -1, -1, 2 }, Vec3f { 1, -1, 2 }, Vec3f { 1, 1, 2 } };
        triangles[1].corner = { Vec3f { -1, -1, 2 }, Vec3f { 1, 1, 2 }, Vec3f { -1, 1, 2 } };
        const float uv0[6] = { 0, 0, 1, 0, 1, 1 }, uv1[6] = { 0, 0, 1, 1, 0, 1 };
        triangles[0].info = triangle_info(TraceClass::foliage, b.add_record(draw, uv0, 2));
        triangles[1].info = triangle_info(TraceClass::foliage, b.add_record(draw, uv1, 2));
        triangles[2].corner = { Vec3f { -5, -5, 4 }, Vec3f { 5, -5, 4 }, Vec3f { 5, 5, 4 } };
        triangles[3].corner = { Vec3f { -5, -5, 4 }, Vec3f { 5, 5, 4 }, Vec3f { -5, 5, 4 } };
        const Bvh bvh = build_bvh(triangles);
        const std::vector<uint32_t> words = b.words(atlas);
        const auto through = [&](float u, float v, uint32_t classes, const std::vector<uint32_t> *alpha) {
            const std::optional<Hit> hit = trace_nearest(bvh, { u * 2 - 1, v * 2 - 1, 0 }, { 0, 0, 1 }, 10, classes, nullptr, alpha);
            return hit ? hit->t : -1.0f;
        };
        const uint32_t all = kAllClasses | kAlphaTests;
        check("through a light square the ray meets the card at 2 m, through a dark one the wall at 4 m",
            through(1.5f / 8, 0.5f / 8, all, &words) == 2 && through(0.5f / 8, 0.5f / 8, all, &words) == 4 && through(4.5f / 8, 5.5f / 8, all, &words) == 2
                && through(5.5f / 8, 5.5f / 8, all, &words) == 4);
        check("without the alpha test, or without the alpha buffer, the card is whole", through(0.5f / 8, 0.5f / 8, kAllClasses, &words) == 2
            && through(0.5f / 8, 0.5f / 8, all, nullptr) == 2);
        check("any hit passes through a dark square too", !trace_any(bvh, { -0.875f, -0.875f, 0 }, { 0, 0, 1 }, 3, all, nullptr, &words)
            && trace_any(bvh, { -0.625f, -0.875f, 0 }, { 0, 0, 1 }, 3, all, nullptr, &words));
        std::mt19937 random(7);
        std::uniform_real_distribution<float> unit(0, 1);
        uint32_t same = 0, light = 0;
        for (int i = 0; i < 2000; i++)
        {
            const Vec3f origin = { unit(random) * 2 - 1, unit(random) * 2 - 1, 0 };
            const Vec3f direction = { unit(random) * 0.4f - 0.2f, unit(random) * 0.4f - 0.2f, 1 };
            const std::optional<Hit> a = trace_nearest(bvh, origin, direction, 10, all, nullptr, &words), c = brute_nearest(bvh, origin, direction, 10, all, &words);
            same += a.has_value() == c.has_value() && (!a || (a->triangle == c->triangle && a->t == c->t));
            light += a && a->t < 3;
        }
        check("2,000 rays through the card: the traversal meets what testing every triangle with its alpha test meets, about half the card",
            same == 2000 && light > 800 && light < 1200);
    }
    std::printf("%s: %d failed\n", __FILE__, failures);
    return failures == 0 ? 0 : 1;
}
