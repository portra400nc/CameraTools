// A shader's Shader Model 4 and 5 instructions, decoded from the SHEX or SHDR chunk of its DXBC container, and its input
// signature. Pure C++17 like dxbc.h. The alpha analysis (core/alpha.*) slices a pixel shader's discards with it.
//
// The token layout is D3D11's d3d11tokenizedprogramformat.hpp, the opcode numbers and names vkd3d's (tpf.c, ir.c), so a
// decoded shader reads as `vkd3d-compiler -b d3d-asm` prints it. Every read is bounds-checked, as in core/dxbc.cpp.
#pragma once

#include "dxbc.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace enhancements
{
    // D3D10_SB_OPERAND_TYPE values the alpha analysis reads.
    namespace operand
    {
        constexpr uint8_t temp = 0, input = 1, output = 2, indexable_temp = 3, immediate32 = 4, immediate64 = 5, sampler = 6, resource = 7,
                          constant_buffer = 8, immediate_constant_buffer = 9, null = 13;
    }

    // Opcodes the alpha analysis treats by name.
    namespace opcode
    {
        constexpr uint32_t add = 0x00, and_ = 0x01, break_ = 0x02, breakc = 0x03, call = 0x04, callc = 0x05, case_ = 0x06, continue_ = 0x07,
                           continuec = 0x08, default_ = 0x0a, discard = 0x0d, div = 0x0e, dp2 = 0x0f, dp3 = 0x10, dp4 = 0x11, else_ = 0x12,
                           endif = 0x15, endloop = 0x16, endswitch = 0x17, eq = 0x18, exp = 0x19, frc = 0x1a, ftoi = 0x1b, ftou = 0x1c, ge = 0x1d,
                           iadd = 0x1e, if_ = 0x1f, ieq = 0x20, ige = 0x21, ilt = 0x22, imad = 0x23, imax = 0x24, imin = 0x25, imul = 0x26,
                           ine = 0x27, ineg = 0x28, ishl = 0x29, ishr = 0x2a, itof = 0x2b, label = 0x2c, log = 0x2f, loop = 0x30, lt = 0x31,
                           mad = 0x32, min = 0x33, max = 0x34, customdata = 0x35, mov = 0x36, movc = 0x37, mul = 0x38, ne = 0x39, nop = 0x3a,
                           not_ = 0x3b, or_ = 0x3c, ret = 0x3e, retc = 0x3f, round_ne = 0x40, round_ni = 0x41, round_pi = 0x42,
                           round_z = 0x43, rsq = 0x44, sample = 0x45, sample_l = 0x48, sample_d = 0x49, sample_b = 0x4a, sqrt = 0x4b,
                           switch_ = 0x4c, ult = 0x4f, uge = 0x50, umax = 0x53, umin = 0x54, ushr = 0x55, utof = 0x56, xor_ = 0x57,
                           dcl_resource = 0x58, dcl_input_ps = 0x62, dcl_input_ps_sgv = 0x63, dcl_input_ps_siv = 0x64, rcp = 0x81;
    }

    // An index of a register: a number, plus the value of a register's component when it is relative (cb1[r0.x + 4]).
    struct ShexIndex
    {
        uint32_t value = 0;
        bool relative = false;
        uint8_t relative_type = 0;      // the register the offset comes from: a temp, an input, an indexable temp
        uint32_t relative_register = 0; // its number; an indexable temp's first index
        uint8_t relative_component = 0; // 0 to 3
    };

    struct ShexOperand
    {
        uint8_t type = operand::null;
        uint8_t components = 0;         // 0, 1 or 4
        // A destination's components, x = 1 to w = 8, in mask mode; 0 for a source.
        uint8_t mask = 0;
        // The component each of a source's four lanes reads; a select-1 or one-component operand reads the same in all four.
        std::array<uint8_t, 4> swizzle { 0, 1, 2, 3 };
        bool negate = false, absolute = false;
        uint8_t dimension = 0;          // how many indices
        std::array<ShexIndex, 3> index {};
        std::array<uint32_t, 4> value {};   // an immediate's bits; one component fills x only
    };

    struct ShexInstruction
    {
        uint32_t opcode = 0;            // bits 0 to 10 of its first token
        uint32_t controls = 0;          // bits 11 to 23: the saturate flag, the test, an interpolation mode, a resource's dimension
        uint8_t resource_dimension = 0; // from an extended resource-dimension token (sample_indexable(texture2d)), else 0
        // A declaration, customdata or a hull shader phase marker. Its operands are not decoded, except dcl_resource's and
        // dcl_input_ps's first.
        bool declaration = false;
        std::vector<ShexOperand> operands;

        bool saturate() const { return (controls >> 2 & 1u) != 0; }         // token bit 13
        bool nonzero() const { return (controls >> 7 & 1u) != 0; }          // token bit 18: if_nz, discard_nz, breakc_nz
        uint32_t interpolation() const { return controls & 15u; }           // dcl_input_ps: 1 constant, 2 linear, ...
        uint32_t declared_dimension() const { return controls & 31u; }      // dcl_resource: 3 texture2d, ...
    };

    struct Shex
    {
        ShaderStageKind stage = ShaderStageKind::ps;
        uint32_t major = 0, minor = 0;
        std::vector<ShexInstruction> instructions;  // in program order, declarations included
    };

    // Empty when the container is broken, has no SHEX or SHDR chunk, or an instruction or operand does not fit in its
    // length or the chunk.
    std::optional<Shex> decode_shex(const uint8_t *code, size_t size);

    // vkd3d's name of an opcode, such as "sample" or "dcl_input_ps", or null when it has none.
    const char *opcode_name(uint32_t opcode);

    // The name with the test and saturate suffixes vkd3d prints: "discard_nz", "mul_sat", "breakp_z". Unknown opcodes read as
    // "op<number>".
    std::string instruction_name(const ShexInstruction &instruction);
}
