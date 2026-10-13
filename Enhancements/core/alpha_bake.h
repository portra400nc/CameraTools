// The alpha tests as the trace kernel runs them: each alpha-tested triangle's coverage mask and cutoff, baked from its
// draw's alpha program (core/alpha.h) when the traced scene is built, so the kernel fetches a mask at a hit and runs no
// program (shaders/alpha_mask.hlsli). Pure C++17 like alpha.h.
//
// A program's kept discards read the point of the triangle through the sample's value and through its other inputs.
// The bake holds the other inputs at the triangle's centroid and finds the sample's value where the verdict changes:
// the cutoff. The mask is the texture's channel the program samples (or its complement, for a test that keeps low
// values), and the record holds the sample's coordinate at the three corners. Where the other inputs are the same over
// the whole triangle (flat, such as the instance a per-instance cutoff reads, or equal at its corners), the mask and
// cutoff give the program's own verdict at every point: exact. Where one varies, such as the world position the leaves'
// distance boost reads, the verdict is the program's at the centroid's value of it: approximate.
//
// The mask buffer, in 32-bit words:
//   header       4 words: kMaskHeaderWords, the records, the first mask's 64 KB slot, the masks
//   records      8 words each: word 0 its mask's slot (bits 0 to 11) and the sampler's address modes (12 to 17), word 1
//                the cutoff as a float, words 2 to 7 the sample's coordinate (u, v) at its three corners
//   masks        from a 64 KB boundary, 256x256 bytes each, a mask a slot
#pragma once

#include "trace_scene.h"

#include <array>
#include <string>
#include <vector>

namespace enhancements
{
    constexpr uint32_t kMaskHeaderWords = 4;
    constexpr uint32_t kMaskRecordWords = 8;
    constexpr uint32_t kMaskSlotBytes = kAtlasTile * kAtlasTile;
    // Records past this are left whole: 128 MB of them, beside at most 512 masks (32 MB), inside the trace's 256 MB buffer.
    constexpr uint32_t kMaxMaskRecords = 4u << 20;

    // How a triangle's alpha test went into its mask, best first. The names follow in kBakeClassNames.
    enum class BakeClass : uint8_t
    {
        exact,          // its discards read the point only through the sample's value: the mask gives the program's verdicts
        approximate,    // they also read an input that varies over the triangle, held at its centroid
        fallback,       // it cannot be baked, and the triangle casts whole
        count,
    };
    extern const std::array<const char *, static_cast<size_t>(BakeClass::count)> kBakeClassNames;

    // What a program's kept discards read, from its code: the attribute floats of a record's corner (bit a for float a)
    // they read other than through the sample's value, and those the sample's coordinate reads.
    struct AlphaFlow
    {
        uint32_t decision = 0;
        uint32_t coordinate = 0;
        bool reads_sample = false;      // the discards read the sample's value
        uint32_t samples = 0;           // sample instructions
    };
    AlphaFlow alpha_flow(const std::vector<std::array<uint32_t, 4>> &code, const std::array<uint8_t, kAlphaInputFloats> &input_map);

    // What the bake makes of a program before it sees a draw: exact when its discards read no interpolated input but
    // through the sample, approximate when they read one, with why naming them ("TEXCOORD1.w ..."). A draw can still do
    // better (an interpolated input equal at a triangle's corners) or worse (a test that does not change once with the
    // sample's value).
    BakeClass bake_class(const AlphaProgram &program, std::string &why);

    // A draw of the bake: its program's offset in the alpha buffer, its triangles by class, and the first fallback's reason.
    struct BakeDraw
    {
        uint32_t program = 0;
        std::array<uint32_t, static_cast<size_t>(BakeClass::count)> triangles {};
        std::string why;
    };

    struct AlphaBakeCounts
    {
        std::array<uint64_t, static_cast<size_t>(BakeClass::count)> triangles {};    // alpha-tested triangles by class
        uint64_t always = 0;        // of the exact and approximate ones, kept everywhere: whole, with no record
        uint64_t never = 0;         // discarded everywhere: a record whose cutoff no sample reaches
        uint64_t past_max = 0;      // whole because the records would pass kMaxMaskRecords
        // Draws by their worst triangle's class, and programs by their worst draw's.
        std::array<uint32_t, static_cast<size_t>(BakeClass::count)> draws {}, programs {};
        uint32_t records = 0, masks = 0;
        std::string why;            // the first fallback's reason
    };

    struct AlphaMasks
    {
        std::vector<uint32_t> words;        // the mask buffer; empty when no triangle has a record
        std::vector<uint32_t> sources;      // each record's program record, for the tests' reference
        std::vector<BakeClass> classes;     // each record's class, exact or approximate
        std::vector<BakeDraw> draws;
        AlphaBakeCounts counts;
    };

    // Bakes the alpha buffer programs (core/alpha.h's layout, as SceneBuilder::take assembles it) for the triangles whose
    // info names a record of it, and rewrites each such info: to its mask record, or to kOpaque where its test keeps
    // every point or cannot be baked. Runs on tasks threads; the result does not depend on how many.
    AlphaMasks bake_alpha(const std::vector<uint32_t> &programs, std::vector<TraceTriangle> &triangles, uint32_t tasks = 1);

    // shaders/alpha_mask.hlsli compiled as C++ (core/kernel.cpp): whether the mask buffer's record keeps a hit at
    // barycentric u and v.
    bool alpha_mask_keeps(const std::vector<uint32_t> &masks, uint32_t record, float u, float v);
}
