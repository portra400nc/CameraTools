// A pixel shader's alpha test as the path tracer runs it. Each discard of the shader's code (core/shex.h) is sliced back
// to what it reads, through the instructions that compute its condition and the ifs that enclose them. A slice that
// reads the pixel's screen position or an immediate constant buffer is screen-door dither and is dropped: the trace
// treats it as passing. The other slices become one small program, which shaders/alpha.hlsli interprets at any hit with
// the game's own semantics, its constants and texture as the draw had them. Pure C++17 like dxbc.h.
//
// The alpha buffer the trace reads holds the traced scene's programs, the draws' constants and texture tiles, and each
// alpha-tested triangle's record. Its layout, in 32-bit words:
//   header       4 words: where the records, programs, draws and atlas start
//   programs     per program: its header (8 words: instructions | literals << 16, attribute floats | flat attributes << 16,
//                the input map's 12 bytes, 3 words of 0), its instructions and its literals, 4 words each
//   draws        per draw: its program, atlas tile, address modes and constant rows (4 words), then its rows, 4 words each
//   records      per triangle: its draw, then its corners' attribute floats, corner by corner
//   atlas        256x256 bytes a tile: the alpha channel of one texture, at the mip whose larger side is at most 256
#pragma once

#include "shex.h"

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace photoreal
{
    // What a discard is to the trace. The names follow in kDiscardKindNames.
    enum class DiscardKind : uint8_t
    {
        texture,        // its condition samples a texture: an alpha test, kept
        dither,         // it reads SV_Position or an immediate constant buffer: screen-door dither, dropped
        other,          // it reads neither, such as a cull by distance from the camera: kept
        unsupported,    // it reads or does what the program cannot: the draw casts its whole triangles
        count,
    };
    extern const std::array<const char *, static_cast<size_t>(DiscardKind::count)> kDiscardKindNames;

    // Components start to start + count - 1 of the element of this semantic and index that the draw's vertex (or domain)
    // shader writes, counted from the element's first component, as DXVK reads a stream-output entry. The scene capture
    // streams them after SV_Position. flat: the pixel shader reads it without interpolation (nointerpolation, or an
    // integer), so the trace takes the first corner's bits.
    struct AlphaInput
    {
        std::string semantic;
        uint32_t index = 0;
        uint8_t start = 0, count = 0;
        bool flat = false;
    };

    // Rows of a constant buffer slot the program reads, copied per draw at replay. A dynamic range, which the code indexes
    // with a register, is the whole range the draw bound, at most kAlphaDynamicRows; only the draw knows how long it is.
    struct CbRows
    {
        uint8_t slot = 0;
        uint16_t first = 0, count = 0;
        bool dynamic = false;
    };

    constexpr uint32_t kAlphaTemps = 8;             // temporaries the interpreter holds
    constexpr uint32_t kAlphaInputFloats = 12;      // interpolated floats, in 3 registers after the temporaries
    constexpr uint32_t kAlphaMaxCode = 48;          // instructions
    constexpr uint32_t kAlphaMaxLiterals = 16;
    constexpr uint32_t kAlphaStaticRows = 64;       // rows of the static ranges together
    constexpr uint32_t kAlphaDynamicRows = 4096;    // 64 KB, the most a constant buffer holds
    constexpr uint8_t kNoSlot = 0xFF;
    constexpr uint32_t kAtlasTile = 256;            // texels a side of an atlas tile
    constexpr uint32_t kNoTile = 0xFFFFu;

    // shaders/alpha.hlsli's operations, which core/kernel.cpp checks against its kOp numbers. Those up to sample take float
    // sources, whose modifiers negate and take the absolute value as floats; and to utof take integer sources.
    namespace alpha_op
    {
        constexpr uint32_t mov = 0, movc = 1, add = 2, mul = 3, mad = 4, div = 5, min = 6, max = 7, dp2 = 8, dp3 = 9, dp4 = 10, lt = 11, ge = 12,
                           eq = 13, ne = 14, ftoi = 15, ftou = 16, round_ne = 17, round_ni = 18, round_pi = 19, round_z = 20, frc = 21, rsq = 22,
                           sqrt = 23, exp = 24, log = 25, rcp = 26, sample = 27, and_ = 28, or_ = 29, xor_ = 30, not_ = 31, iadd = 32, ieq = 33,
                           ine = 34, ilt = 35, ige = 36, ult = 37, uge = 38, imin = 39, imax = 40, umin = 41, umax = 42, ineg = 43, ishl = 44,
                           ishr = 45, ushr = 46, itof = 47, utof = 48, if_nz = 60, if_z = 61, else_ = 62, discard_nz = 63, discard_z = 64;
    }

    // The program's instructions, in shaders/alpha.hlsli's encoding: per instruction x holds the operation (bits 0 to 7),
    // the destination temporary (8 to 11), its mask (12 to 15), saturate (16) and an if's or else's jump target (20 to 31);
    // y to w the sources, each its kind (bits 0 and 1: 0 a register, 1 a constant row, 2 a constant row offset by a
    // register's component, 3 a literal), swizzle (2 to 9), negate (10), absolute (11), the offset's register (12 to 15)
    // and component (16 and 17), and its index (18 to 31). Registers 0 to 7 are temporaries, 8 to 10 the interpolated inputs.
    struct AlphaProgram
    {
        std::vector<std::array<uint32_t, 4>> code;
        std::vector<std::array<uint32_t, 4>> literals;
        std::vector<AlphaInput> inputs;             // their components in order are the record's attribute floats
        std::array<uint8_t, kAlphaInputFloats> input_map {};    // each input register component's attribute float, 0xFF for none
        std::vector<CbRows> rows;                   // static ranges in slot order, then at most one dynamic range
        uint8_t texture = kNoSlot, sampler = kNoSlot;   // the one texture it samples, and its sampler
        uint8_t channel = 0;                        // the texture's channel it reads, 0 = x to 3 = w

        uint32_t attribute_floats() const;
        // Rows of the static ranges, which come first in a draw's constants.
        uint32_t static_rows() const;
        // The program's words in the alpha buffer: its header, instructions and literals.
        std::vector<uint32_t> words() const;
        // The stream-output entries that follow the position, in slot 0.
        std::vector<SoEntry> so_entries() const;
        // "TEXCOORD0.xy TEXCOORD1.w" for draws.tsv.
        std::string describe() const;
    };

    struct AlphaAnalysis
    {
        std::vector<DiscardKind> discards;              // each discard, in the code's order
        std::shared_ptr<const AlphaProgram> program;    // when a discard is kept and every kept one is supported
        std::string why;                                // the first unsupported discard's reason, as "loop" or "opcode gather4"
    };

    // A pixel shader's bytecode. A shader without discard, or whose discards are all dither, has no program and is opaque;
    // one that cannot be decoded has none either, with why "decode".
    AlphaAnalysis analyze_alpha(const uint8_t *code, size_t size);

    // Why the interpreter could not run program within its caps, or empty when it can: 1 to kAlphaMaxCode instructions of
    // known operations, each if's and else's jump forward past itself and at most to the end, so the interpreter runs at most
    // one step an instruction; destinations among the temporaries, registers among the temporaries and inputs, and literals
    // among the program's. analyze_alpha keeps no program that fails it.
    std::string check_alpha_program(const AlphaProgram &program);

    // shaders/alpha.hlsli compiled as C++ (core/kernel.cpp): whether the alpha test keeps a hit at barycentric u and v of
    // the triangle whose record starts at word record of the records, in an alpha buffer of the layout above.
    bool alpha_keeps(const std::vector<uint32_t> &alpha, uint32_t record, float u, float v);

    // A triangle's info word holds its record's offset in bits 0 to 23, this for none: a whole triangle.
    constexpr uint32_t kNoRecord = 0xFFFFFFu;

    // Assembles an alpha buffer of the layout above. Programs are kept once each; draws and records in the order they come.
    class AlphaBuffer
    {
    public:
        // Its offset among the programs, the same for the same program.
        uint32_t add_program(const std::shared_ptr<const AlphaProgram> &program);
        // A draw of program (add_program's offset) with its texture's atlas tile (kNoTile for none), its sampler's address
        // modes (D3D11_TEXTURE_ADDRESS_MODE, u in bits 0 to 2, v in 3 to 5) and its constant rows, 4 words each: its offset
        // among the draws.
        uint32_t add_draw(uint32_t program, uint32_t tile, uint32_t address, const std::vector<uint32_t> &rows);
        // A triangle of draw (add_draw's offset) with its three corners' attribute floats, corner by corner: its record's
        // offset among the records, or kNoRecord once they would pass kNoRecord words.
        uint32_t add_record(uint32_t draw, const float *corners, uint32_t floats);
        // The buffer, with atlas after the records, padded to whole words.
        std::vector<uint32_t> words(const std::vector<uint8_t> &atlas) const;
        uint64_t record_words() const { return records_.size(); }

    private:
        std::vector<std::pair<const AlphaProgram *, uint32_t>> program_offsets_;
        std::vector<uint32_t> programs_, draws_, records_;
    };
}
