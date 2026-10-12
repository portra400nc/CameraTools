// The alpha analysis of census pixel shaders (tests/fixtures/<hash>.ps.dxbc): each discard's kind, the program's inputs,
// constant rows, texture and stream-output entries, as alpha_census.py found them; the interpreter (shaders/alpha.hlsli
// compiled as C++) on the leaf shader 0a48f0bb's program against the shader's own arithmetic worked by hand, on the
// per-instance cutoff of 5e9447eb with its flat instance and dynamically indexed row; the bake of programs into coverage
// masks (core/alpha_bake.cpp) against the programs' verdicts; and rays through cards with baked masks.
#include "../core/alpha_bake.h"
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

    float as_float(uint32_t u)
    {
        float f;
        std::memcpy(&f, &u, 4);
        return f;
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
        bool all_valid = true;
        for (const AlphaAnalysis *a : { &leaves, &fade, &instanced, &culled, &grass, &facing, &cutoff, &prepass })
            all_valid = all_valid && a->program && check_alpha_program(*a->program).empty();
        check("every census program checks: forward jumps within it, registers, destinations and literals in range", all_valid);
    }
    {
        // Programs the interpreter could not run within its caps, each refused with its reason: an if to itself, an if back,
        // an else past the end, a jump on an add, an unknown operation, a destination past the temporaries, a register past
        // the inputs, a literal the program lacks, and 49 instructions. A forward if to the end passes.
        const auto make = [](std::vector<std::array<uint32_t, 4>> code, uint32_t literals = 0) {
            AlphaProgram p;
            p.code = std::move(code);
            p.literals.resize(literals);
            return p;
        };
        const auto op = [](uint32_t o, uint32_t target = 0, uint32_t dst = 0) { return o | dst << 8 | 15u << 12 | target << 20; };
        const std::array<uint32_t, 4> add = { op(alpha_op::add), 0, 0, 0 }, discard = { op(alpha_op::discard_nz), 0, 0, 0 };
        check("a program whose if jumps forward to its end passes", check_alpha_program(make({ { op(alpha_op::if_nz, 3), 0, 0, 0 }, add, discard })).empty());
        const std::pair<const char *, AlphaProgram> broken[] = {
            { "an if that jumps to itself", make({ add, { op(alpha_op::if_nz, 1), 0, 0, 0 }, discard }) },
            { "an if that jumps back", make({ add, discard, { op(alpha_op::if_z, 0), 0, 0, 0 } }) },
            { "an else past the end", make({ { op(alpha_op::else_, 4), 0, 0, 0 }, add, discard }) },
            { "an add with a jump target", make({ { op(alpha_op::add, 2), 0, 0, 0 }, discard }) },
            { "operation 50", make({ { op(50), 0, 0, 0 }, discard }) },
            { "destination r8", make({ { op(alpha_op::mov, 0, 8), 0, 0, 0 }, discard }) },
            { "register 11, a source register", make({ { op(alpha_op::mov), 11u << 18, 0, 0 }, discard }) },
            { "literal 1 of 1", make({ { op(alpha_op::mov), 3u | 1u << 18, 0, 0 }, discard }, 1) },
            { "49 instructions", make(std::vector<std::array<uint32_t, 4>>(49, add)) },
        };
        for (const auto &[what, program] : broken)
        {
            const std::string why = check_alpha_program(program);
            check(std::string("refused: ") + what + " (" + why + ")", !why.empty());
        }
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
        // Baked (core/alpha_bake.cpp): with its corners all at the origin, the distance the boost reads is the same over the
        // triangle, so the mask is the tile and the cutoff 0.4, where t0.z plus 0.1 reaches 0.5, and its verdicts are the
        // program's. With its corners 40 m apart the distance varies over it: approximate, the cutoff the centroid's.
        const auto [exact_words, exact_record] = buffer(10, kWrap, 1);
        std::vector<TraceTriangle> one(1);
        one[0].info = triangle_info(TraceClass::foliage, exact_record);
        const AlphaMasks exact = bake_alpha(exact_words, one);
        const float exact_cutoff = as_float(exact.words.size() > kMaskHeaderWords + 1 ? exact.words[kMaskHeaderWords + 1] : 0);
        const auto agree = [](const std::vector<uint32_t> &programs, uint32_t record, const AlphaMasks &masks) {
            uint32_t same = 0, points = 0;
            for (int i = 0; i < 40; i++)
                for (int j = 0; i + j < 40; j++, points++)
                {
                    const float u = (i + 0.37f) / 41, v = (j + 0.61f) / 41;
                    same += alpha_keeps(programs, record, u, v) == alpha_mask_keeps(masks.words, 0, u, v);
                }
            return std::pair { same, points };
        };
        const auto [exact_same, exact_points] = agree(exact_words, exact_record, exact);
        check("baked with its corners at one spot: exact, one record of the tile with the cutoff " + std::to_string(exact_cutoff)
                + ", and its verdicts the program's at all " + std::to_string(exact_points) + " points",
            exact.counts.triangles[0] == 1 && exact.counts.records == 1 && exact.counts.masks == 1 && std::fabs(exact_cutoff - 0.4f) < 1e-4f
                && (one[0].info & kOpaque) == 0 && exact_same == exact_points);
        AlphaBuffer spread;
        std::vector<uint32_t> constants;
        append(constants, row(10, 0, 0, 0));
        append(constants, row(0, 20, 0.2f, 0));
        for (int i = 0; i < 3; i++)
            append(constants, row(0, 0, 0, 0));
        append(constants, row(0, 0, 0, 0.5f));
        const uint32_t spread_draw = spread.add_draw(spread.add_program(leaves.program), 0, address(kWrap), constants);
        const float spread_corners[15] = { 0, 0, 0, 0, 0, 1, 0, 40, 0, 0, 0, 1, 0, 40, 0 };
        const uint32_t spread_record = spread.add_record(spread_draw, spread_corners, 5);
        const std::vector<uint32_t> spread_words = spread.words(atlas);
        one[0].info = triangle_info(TraceClass::foliage, spread_record);
        const AlphaMasks approximate = bake_alpha(spread_words, one);
        const float approximate_cutoff = as_float(approximate.words.size() > kMaskHeaderWords + 1 ? approximate.words[kMaskHeaderWords + 1] : 0);
        // At the centroid, (13.3, 13.3, 0), 13.7 m from the camera: 0.5 - 0.2 x 13.7 / 20.
        const float centroid = 0.5f - 0.2f * std::sqrt(3.3333f * 3.3333f + 13.3333f * 13.3333f) / 20.0001f;
        const auto [spread_same, spread_points] = agree(spread_words, spread_record, approximate);
        check("baked with its corners 40 m apart: approximate, the cutoff " + std::to_string(approximate_cutoff) + " the centroid's "
                + std::to_string(centroid) + ", and " + std::to_string(spread_same) + " of " + std::to_string(spread_points) + " verdicts the program's",
            approximate.counts.triangles[1] == 1 && std::fabs(approximate_cutoff - centroid) < 1e-3f && spread_same < spread_points
                && spread_same > spread_points * 9 / 10);
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
        // Baked: the instance is flat, so each triangle's cutoff is its own instance's, exact. Instance 1 keeps the tile's 0.502
        // against 0.25, instance 3 does not against 0.75, and the row past the draw's keeps every point and goes whole.
        std::vector<TraceTriangle> three_triangles(3);
        three_triangles[0].info = triangle_info(TraceClass::foliage, one);
        three_triangles[1].info = triangle_info(TraceClass::foliage, three);
        three_triangles[2].info = triangle_info(TraceClass::foliage, past);
        const AlphaMasks masks = bake_alpha(words, three_triangles);
        check("baked: all three exact, two records with the cutoffs 0.25 and 0.75 and one mask, the third whole as it keeps every point",
            masks.counts.triangles[0] == 3 && masks.counts.always == 1 && masks.counts.records == 2 && masks.counts.masks == 1
                && std::fabs(as_float(masks.words[kMaskHeaderWords + 1]) - 0.25f) < 1e-6f
                && std::fabs(as_float(masks.words[kMaskHeaderWords + kMaskRecordWords + 1]) - 0.75f) < 1e-6f
                && (three_triangles[2].info & kOpaque) == kOpaque && alpha_mask_keeps(masks.words, 0, 0.4f, 0.4f)
                && !alpha_mask_keeps(masks.words, 1, 0.4f, 0.4f) && !alpha_mask_keeps(masks.words, 1, 0.0f, 0.0f));
    }
    {
        // A 2 m card at z = 2 in front of a wall at z = 4, alpha-tested by 68b05754 (t0.w against cb2[14].y = 0.5) with a
        // checkerboard of 8 by 8 squares, baked: a ray through a dark square meets the wall, one through a light square the card.
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
        const std::vector<uint32_t> programs = b.words(atlas);
        const AlphaMasks masks = bake_alpha(programs, triangles);
        const Bvh bvh = build_bvh(triangles);
        const std::vector<uint32_t> &words = masks.words;
        // The buffer: the header, two records from a 64 KB boundary's first slot, and the tile there.
        const uint32_t slot = words.size() > kMaskHeaderWords ? words[kMaskHeaderWords] & 4095 : 0;
        check("the mask buffer: 2 records, then the tile in slot 1, a record's slot, address modes, cutoff 0.5 and corners' coordinates",
            words.size() == 2 * kMaskSlotBytes / 4 && words[0] == kMaskHeaderWords && words[1] == 2 && words[2] == 1 && words[3] == 1 && slot == 1
                && (words[kMaskHeaderWords] >> 12) == address(kClamp) && as_float(words[kMaskHeaderWords + 1]) == 0.5f
                && as_float(words[kMaskHeaderWords + 4]) == 1 && as_float(words[kMaskHeaderWords + 7]) == 1
                && as_float(words[kMaskHeaderWords + kMaskRecordWords + 5]) == 1 && words[slot * kMaskSlotBytes / 4 + 8] == 0xFFFFFFFFu
                && words[slot * kMaskSlotBytes / 4] == 0 && masks.sources[0] == 0 && masks.classes[1] == BakeClass::exact);
        const auto through = [&](float u, float v, uint32_t classes, const std::vector<uint32_t> *alpha) {
            const std::optional<Hit> hit = trace_nearest(bvh, { u * 2 - 1, v * 2 - 1, 0 }, { 0, 0, 1 }, 10, classes, nullptr, alpha);
            return hit ? hit->t : -1.0f;
        };
        const uint32_t all = kAllClasses | kAlphaTests;
        check("through a light square the ray meets the card at 2 m, through a dark one the wall at 4 m",
            through(1.5f / 8, 0.5f / 8, all, &words) == 2 && through(0.5f / 8, 0.5f / 8, all, &words) == 4 && through(4.5f / 8, 5.5f / 8, all, &words) == 2
                && through(5.5f / 8, 5.5f / 8, all, &words) == 4);
        check("without the alpha test, or without the mask buffer, the card is whole", through(0.5f / 8, 0.5f / 8, kAllClasses, &words) == 2
            && through(0.5f / 8, 0.5f / 8, all, nullptr) == 2);
        check("any hit passes through a dark square too", !trace_any(bvh, { -0.875f, -0.875f, 0 }, { 0, 0, 1 }, 3, all, nullptr, &words)
            && trace_any(bvh, { -0.625f, -0.875f, 0 }, { 0, 0, 1 }, 3, all, nullptr, &words));
        const auto program = [&](uint32_t record, float u, float v) { return alpha_keeps(programs, masks.sources[record], u, v); };
        std::mt19937 random(7);
        std::uniform_real_distribution<float> unit(0, 1);
        uint32_t same = 0, as_program = 0, light = 0;
        for (int i = 0; i < 2000; i++)
        {
            const Vec3f origin = { unit(random) * 2 - 1, unit(random) * 2 - 1, 0 };
            const Vec3f direction = { unit(random) * 0.4f - 0.2f, unit(random) * 0.4f - 0.2f, 1 };
            const std::optional<Hit> a = trace_nearest(bvh, origin, direction, 10, all, nullptr, &words), c = brute_nearest(bvh, origin, direction, 10, all, &words);
            const std::optional<Hit> p = brute_nearest(bvh, origin, direction, 10, all, program);
            same += a.has_value() == c.has_value() && (!a || (a->triangle == c->triangle && a->t == c->t));
            as_program += a.has_value() == p.has_value() && (!a || (a->triangle == p->triangle && a->t == p->t));
            light += a && a->t < 3;
        }
        check("2,000 rays through the card: the traversal meets what testing every triangle with its mask meets, and what testing it with the "
              "program meets, about half the card",
            same == 2000 && as_program == 2000 && light > 800 && light < 1200);
    }
    {
        // A ray's alpha cap (shaders/bvh.hlsli): a stack of 80 cards 0.05 m apart in front of a wall at 5 m, each alpha-tested
        // by 68b05754 against a tile of zeros, so every card discards everywhere and the ray would test all 80 and meet the wall.
        const AlphaAnalysis cutoff = analyze("68b05754fdce207c.ps.dxbc");
        const auto stack = [&](const std::shared_ptr<const AlphaProgram> &program, std::vector<uint32_t> &words, AlphaBakeCounts &counts) {
            AlphaBuffer b;
            std::vector<uint32_t> constants;
            append(constants, row(0, 0.5f, 0, 0));
            const uint32_t draw = b.add_draw(b.add_program(program), 0, address(kClamp), constants);
            std::vector<TraceTriangle> triangles;
            const float uv[6] = { 0, 0, 1, 0, 0, 1 };
            for (int k = 1; k <= 80; k++)
            {
                const float z = 0.05f * k;
                TraceTriangle t;
                t.corner = { Vec3f { -1, -1, z }, Vec3f { 3, -1, z }, Vec3f { -1, 3, z } };
                t.info = triangle_info(TraceClass::foliage, b.add_record(draw, uv, 2));
                triangles.push_back(t);
            }
            TraceTriangle wall;
            wall.corner = { Vec3f { -5, -5, 5 }, Vec3f { 5, -5, 5 }, Vec3f { 0, 5, 5 } };
            triangles.push_back(wall);
            AlphaMasks masks = bake_alpha(b.words(std::vector<uint8_t>(kAtlasTile * kAtlasTile, 0)), triangles);
            words = std::move(masks.words);
            counts = masks.counts;
            return build_bvh(triangles);
        };
        const uint32_t all = kAllClasses | kAlphaTests;
        std::vector<uint32_t> words;
        AlphaBakeCounts counts;
        const Bvh cards = stack(cutoff.program, words, counts);
        TraversalStats stats;
        const std::optional<Hit> capped = trace_nearest(cards, { 0, 0, 0 }, { 0, 0, 1 }, 10, all, &stats, &words);
        const std::optional<Hit> brute = brute_nearest(cards, { 0, 0, 0 }, { 0, 0, 1 }, 10, all, &words);
        check("through 80 cards that discard everywhere, a ray makes 64 alpha tests, meets the guard and then a card counted whole, where testing "
              "every triangle meets the wall at 5 m",
            stats.alpha_tests == 64 && stats.guard && capped && capped->t < 4.05f && brute && brute->t == 5);
        // 19 moves of a literal 1 into r0 and a discard where r0.x is not 0: no sample, discarded everywhere, so each card is a
        // record whose cutoff no sample reaches.
        AlphaProgram always_discards;
        always_discards.literals = { { 1, 1, 1, 1 } };
        for (int i = 0; i < 19; i++)
            always_discards.code.push_back({ alpha_op::mov | 15u << 12, 3u, 0, 0 });
        always_discards.code.push_back({ alpha_op::discard_nz, 0, 0, 0 });
        const Bvh never = stack(std::make_shared<const AlphaProgram>(always_discards), words, counts);
        const std::optional<Hit> past = trace_nearest(never, { 0, 0, 0 }, { 0, 0, 1 }, 10, all, &stats, &words);
        check("a program without a sample that discards everywhere bakes to 80 records of cutoff 2, and the ray meets a card after 64 tests",
            counts.never == 80 && counts.records == 80 && counts.masks == 0 && as_float(words[kMaskHeaderWords + 1]) == 2 && stats.alpha_tests == 64
                && stats.guard && past && past->t < 4.05f);
        // 49 instructions, past the interpreter's cap, which the analysis would refuse but a buffer can still hold.
        AlphaProgram too_long = always_discards;
        too_long.code.insert(too_long.code.begin(), 29, too_long.code[0]);
        const Bvh refused = stack(std::make_shared<const AlphaProgram>(too_long), words, counts);
        const std::optional<Hit> whole = trace_nearest(refused, { 0, 0, 0 }, { 0, 0, 1 }, 10, all, &stats, &words);
        check("a program of 49 instructions cannot be baked: its 80 cards fall back whole, and the ray meets the first with no alpha test",
            counts.triangles[2] == 80 && counts.draws[2] == 1 && counts.programs[2] == 1 && counts.why == "a program the interpreter cannot run"
                && words.empty() && whole && std::fabs(whole->t - 0.05f) < 1e-6f && stats.alpha_tests == 0);
    }
    {
        // Programs the bake holds in other ways, each on one triangle with its UV from its corners and a tile whose texel x
        // holds x: a sample, then a discard below 0.3 and one above 0.7, which changes the verdict twice with the sample and
        // cannot be baked; a discard above 0.6 alone, which keeps low samples, baked as the complement of the tile with the
        // cutoff 0.4; and two draws of one tile and one program, whose triangles share a mask.
        const auto ins = [](uint32_t op, uint32_t dst = 0, uint32_t mask = 15) { return std::array<uint32_t, 4> { op | dst << 8 | mask << 12, 0, 0, 0 }; };
        const auto reg = [](uint32_t r, uint32_t swizzle = 0) { return r << 18 | swizzle << 2; };
        const auto literal = [](uint32_t l) { return 3u | l << 18; };
        const uint32_t xyxx = 0 | 1 << 2;
        const auto program_of = [&](std::vector<std::array<uint32_t, 4>> code, std::vector<std::array<uint32_t, 4>> literals) {
            AlphaProgram p;
            p.code = std::move(code);
            p.literals = std::move(literals);
            p.inputs = { { "TEXCOORD", 0, 0, 2, false } };
            p.input_map.fill(0xFF);
            p.input_map[0] = 0;
            p.input_map[1] = 1;
            return std::make_shared<const AlphaProgram>(std::move(p));
        };
        const auto lit = [](float f) { const uint32_t b = bits(f); return std::array<uint32_t, 4> { b, b, b, b }; };
        // r0 = sample(v8.xy); r1 = r0 < 0.3; discard r1; r1 = 0.7 < r0; discard r1.
        auto band = program_of({ ins(alpha_op::sample), ins(alpha_op::lt, 1), ins(alpha_op::discard_nz), ins(alpha_op::lt, 1), ins(alpha_op::discard_nz) },
            { lit(0.3f), lit(0.7f) });
        auto &band_code = const_cast<AlphaProgram &>(*band).code;
        band_code[0][1] = reg(8, xyxx);
        band_code[1][1] = reg(0);
        band_code[1][2] = literal(0);
        band_code[2][1] = reg(1);
        band_code[3][1] = literal(1);
        band_code[3][2] = reg(0);
        band_code[4][1] = reg(1);
        // r0 = sample(v8.xy); r1 = 0.6 < r0; discard r1.
        auto low = program_of({ ins(alpha_op::sample), ins(alpha_op::lt, 1), ins(alpha_op::discard_nz) }, { lit(0.6f) });
        auto &low_code = const_cast<AlphaProgram &>(*low).code;
        low_code[0][1] = reg(8, xyxx);
        low_code[1][1] = literal(0);
        low_code[1][2] = reg(0);
        low_code[2][1] = reg(1);
        check("the hand-made programs check", check_alpha_program(*band).empty() && check_alpha_program(*low).empty());
        std::vector<uint8_t> atlas(kAtlasTile * kAtlasTile);
        for (uint32_t i = 0; i < atlas.size(); i++)
            atlas[i] = static_cast<uint8_t>(i % kAtlasTile);
        AlphaBuffer b;
        const float uv[6] = { 0, 0, 1, 0, 0, 1 };
        std::vector<TraceTriangle> triangles(4);
        triangles[0].info = triangle_info(TraceClass::foliage, b.add_record(b.add_draw(b.add_program(band), 0, address(kWrap), {}), uv, 2));
        const uint32_t low_draw = b.add_draw(b.add_program(low), 0, address(kWrap), {});
        triangles[1].info = triangle_info(TraceClass::foliage, b.add_record(low_draw, uv, 2));
        triangles[2].info = triangle_info(TraceClass::foliage, b.add_record(b.add_draw(b.add_program(low), 0, address(kClamp), {}), uv, 2));
        triangles[3].info = triangle_info(TraceClass::foliage, b.add_record(low_draw, uv, 2));
        const std::vector<uint32_t> programs = b.words(atlas);
        const AlphaMasks masks = bake_alpha(programs, triangles);
        const uint32_t first = masks.words.size() > 2 ? masks.words[2] * kMaskSlotBytes / 4 : 0;
        bool complement = masks.words.size() >= first + kMaskSlotBytes / 4;
        for (uint32_t k = 0; complement && k < kMaskSlotBytes; k++)
            complement = (masks.words[first + k / 4] >> (k % 4 * 8) & 255) == 255u - atlas[k];
        uint32_t same = 0, points = 0;
        for (uint32_t record = 0; record < masks.counts.records; record++)
            for (int i = 0; i < 30; i++)
                for (int j = 0; i + j < 30; j++, points++)
                {
                    const float u = (i + 0.37f) / 31, v = (j + 0.61f) / 31;
                    same += alpha_keeps(programs, masks.sources[record], u, v) == alpha_mask_keeps(masks.words, record, u, v);
                }
        check("the band cannot be baked and goes whole; the low test is exact, the tile's complement with the cutoff 0.4, one mask for its three "
              "triangles of two draws, and " + std::to_string(same) + " of " + std::to_string(points) + " verdicts the program's",
            masks.counts.triangles[2] == 1 && masks.counts.triangles[0] == 3 && masks.counts.why == "a verdict that changes more than once with the sample"
                && (triangles[0].info & kOpaque) == kOpaque && masks.counts.records == 3 && masks.counts.masks == 1 && complement
                && std::fabs(as_float(masks.words[kMaskHeaderWords + 1]) - 0.4f) < 1e-6f && same == points);
        std::string why;
        check("before a draw: both programs exact, as they read nothing but the sample", bake_class(*band, why) == BakeClass::exact
            && bake_class(*low, why) == BakeClass::exact && why.empty());
        const AlphaAnalysis leaves = analyze("0a48f0bb29e3c15d.ps.dxbc"), instanced = analyze("5e9447eb5599aa41.ps.dxbc");
        check("the leaves' 0a48f0bb approximate, as its distance boost reads the world position; 5e9447eb exact, its instance flat",
            bake_class(*leaves.program, why) == BakeClass::approximate && why == "TEXCOORD1.w TEXCOORD2.w TEXCOORD3.w"
                && bake_class(*instanced.program, why) == BakeClass::exact);
    }
    {
        // The work cap: 6,000 triangles across the ray's path whose boxes it passes through but which it misses, each the
        // lower part of a square cut along a line under the ray, then a wall at 10 m. The ray tests them all until its
        // work, nodes and triangles together, reaches 4,096, and stops there, short of the wall.
        std::vector<TraceTriangle> triangles;
        for (int i = 0; i < 6000; i++)
        {
            const float z = 0.001f * (i + 1);
            TraceTriangle t;
            t.corner = { Vec3f { -1, -1, z }, Vec3f { 1, -1, z }, Vec3f { 1, 0.9f, z } };
            triangles.push_back(t);
        }
        TraceTriangle wall;
        wall.corner = { Vec3f { -5, -5, 10 }, Vec3f { 5, -5, 10 }, Vec3f { 0, 5, 10 } };
        triangles.push_back(wall);
        const Bvh bvh = build_bvh(triangles);
        TraversalStats stats;
        const std::optional<Hit> hit = trace_nearest(bvh, { 0, 0, 0 }, { 0, 0, 1 }, 20, kAllClasses, &stats);
        check("a ray whose boxes hold 6,000 triangles it misses stops at the work cap: work " + std::to_string(stats.work) + " of 4,096 + 32, "
                + std::to_string(stats.steps) + " nodes, guard",
            stats.guard && stats.work >= 4096 && stats.work <= 4096 + 32 && stats.alpha_tests == 0 && (!hit || hit->t == 10)
                && brute_nearest(bvh, { 0, 0, 0 }, { 0, 0, 1 }, 20) && brute_nearest(bvh, { 0, 0, 0 }, { 0, 0, 1 }, 20)->t == 10);
    }
    {
        // tests/fixtures/scene-085011-alpha-rays.bin, which make_alpha_fixture.cpp cut from build 203's capture at the spot
        // where build 204 hung the Deck's GPU: 64 rays of the first 4 tiles of its frame, the 32 crossing the most foliage
        // cards and 32 others, and the 469 triangles near them. Its cards get 68b05754's program, baked.
        std::ifstream file("fixtures/scene-085011-alpha-rays.bin", std::ios::binary);
        const std::string bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        std::vector<uint32_t> w(bytes.size() / 4);
        std::memcpy(w.data(), bytes.data(), w.size() * 4);
        const auto f = [&](size_t i) { return as_float(w[i]); };
        struct FixtureRay
        {
            Vec3f o, d;
            float t_max;
            bool any_hit;
            uint32_t mask;
        };
        std::vector<FixtureRay> rays;
        std::vector<TraceTriangle> triangles;
        if (w.size() > 3 && w[0] == 0x46414850u && w[1] == 1)
        {
            size_t at = 3;
            for (uint32_t i = 0; i < w[2] && at + 9 <= w.size(); i++, at += 9)
                rays.push_back({ { f(at), f(at + 1), f(at + 2) }, { f(at + 3), f(at + 4), f(at + 5) }, f(at + 6), w[at + 7] != 0, w[at + 8] });
            const uint32_t count = at < w.size() ? w[at++] : 0;
            for (uint32_t i = 0; i < count && at + 10 <= w.size(); i++, at += 10)
            {
                TraceTriangle t;
                t.corner = { Vec3f { f(at), f(at + 1), f(at + 2) }, Vec3f { f(at + 3), f(at + 4), f(at + 5) }, Vec3f { f(at + 6), f(at + 7), f(at + 8) } };
                t.info = triangle_info(static_cast<TraceClass>(w[at + 9]));
                triangles.push_back(t);
            }
        }
        check("the Deck spot's fixture holds 64 rays and 469 triangles", rays.size() == 64 && triangles.size() == 469);
        const AlphaAnalysis cutoff = analyze("68b05754fdce207c.ps.dxbc");
        struct Carded
        {
            Bvh bvh;
            uint32_t cards = 0;
            std::vector<uint32_t> programs;
            AlphaMasks masks;
        };
        const auto with_atlas = [&](const std::vector<uint8_t> &atlas) {
            AlphaBuffer b;
            std::vector<uint32_t> constants;
            append(constants, row(0, 0.5f, 0, 0));
            const uint32_t draw = b.add_draw(b.add_program(cutoff.program), 0, address(kWrap), constants);
            std::vector<TraceTriangle> carded = triangles;
            Carded out;
            const float uv[6] = { 0, 0, 1, 0, 0, 1 };
            for (TraceTriangle &t : carded)
                if (info_class(t.info) == TraceClass::vegetation || info_class(t.info) == TraceClass::foliage)
                {
                    t.info = triangle_info(info_class(t.info), b.add_record(draw, uv, 2));
                    out.cards++;
                }
            out.programs = b.words(atlas);
            out.masks = bake_alpha(out.programs, carded);
            out.bvh = build_bvh(carded);
            return out;
        };
        // Every card discarding: each ray tests each card nearer than what it meets, all within the caps.
        const Carded clear = with_atlas(std::vector<uint8_t>(kAtlasTile * kAtlasTile, 0));
        uint32_t most = 0, guards = 0, total = 0;
        for (const FixtureRay &r : rays)
        {
            TraversalStats stats;
            if (r.any_hit)
                trace_any(clear.bvh, r.o, r.d, r.t_max, r.mask, &stats, &clear.masks.words);
            else
                trace_nearest(clear.bvh, r.o, r.d, r.t_max, r.mask, &stats, &clear.masks.words);
            most = std::max(most, stats.alpha_tests);
            guards += stats.guard;
            total += stats.alpha_tests;
        }
        check("with every one of its " + std::to_string(clear.cards) + " cards discarding, its rays make " + std::to_string(total) + " alpha tests, at most "
                + std::to_string(most) + " a ray, within the caps, and no ray meets a guard",
            clear.cards == 443 && clear.masks.counts.records == 443 && total == 731 && most == 20 && guards == 0);
        // Leaf shapes: blobs of a texture about 43% opaque. Every card bakes exactly, and each ray meets what testing every
        // triangle with its mask meets, and what testing it with the program meets.
        std::vector<uint8_t> atlas(kAtlasTile * kAtlasTile);
        for (uint32_t i = 0; i < atlas.size(); i++)
            atlas[i] = ((i % kAtlasTile / 16) * 5 + (i / kAtlasTile / 16) * 3) % 7 < 3 ? 255 : 0;
        const Carded leaves = with_atlas(atlas);
        const auto program = [&](uint32_t record, float u, float v) { return alpha_keeps(leaves.programs, leaves.masks.sources[record], u, v); };
        uint32_t same = 0, as_program = 0, shadowed = 0;
        guards = 0;
        for (const FixtureRay &r : rays)
        {
            TraversalStats stats;
            const std::optional<Hit> brute = brute_nearest(leaves.bvh, r.o, r.d, r.t_max, r.mask, &leaves.masks.words);
            const std::optional<Hit> reference = brute_nearest(leaves.bvh, r.o, r.d, r.t_max, r.mask, program);
            if (r.any_hit)
            {
                const bool any = trace_any(leaves.bvh, r.o, r.d, r.t_max, r.mask, &stats, &leaves.masks.words);
                same += any == brute.has_value();
                as_program += any == reference.has_value();
                shadowed += any;
            }
            else
            {
                const std::optional<Hit> hit = trace_nearest(leaves.bvh, r.o, r.d, r.t_max, r.mask, &stats, &leaves.masks.words);
                same += hit.has_value() == brute.has_value() && (!hit || (hit->triangle == brute->triangle && hit->t == brute->t));
                as_program += hit.has_value() == reference.has_value() && (!hit || (hit->triangle == reference->triangle && hit->t == reference->t));
            }
            guards += stats.guard;
        }
        check("with leaf-shaped cards, all 443 baked exactly, all 64 rays meet what testing every triangle with its mask meets and what testing it "
              "with the program meets (" + std::to_string(shadowed) + " sun rays shadowed), and no ray meets a guard",
            leaves.masks.counts.triangles[0] == 443 && same == 64 && as_program == 64 && shadowed == 48 && guards == 0);
    }
    std::printf("%s: %d failed\n", __FILE__, failures);
    return failures == 0 ? 0 : 1;
}
