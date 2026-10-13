// The SHEX decoder on census pixel shaders (tests/fixtures/<hash>.ps.dxbc as the census dumped them): instruction names
// as vkd3d-compiler's d3d-asm prints them, operands of a dynamically indexed constant buffer and an indexable sample, the
// input signature, and containers cut short. check_shex.sh compares every census shader with vkd3d's disassembly.
#include "../core/shex.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

using namespace enhancements;

namespace
{
    int failures = 0;

    void check(const char *name, bool ok)
    {
        std::printf("%s %s\n", ok ? "PASS" : "FAIL", name);
        failures += ok ? 0 : 1;
    }

    std::vector<uint8_t> load(const std::string &name)
    {
        std::ifstream file("fixtures/" + name, std::ios::binary);
        std::vector<uint8_t> data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        if (data.empty())
            throw std::runtime_error("missing fixtures/" + name);
        return data;
    }

    // The instructions' names, declarations left out, as vkd3d prints them without its resource and precise qualifiers.
    std::string names(const std::vector<uint8_t> &code)
    {
        const std::optional<Shex> shex = decode_shex(code.data(), code.size());
        if (!shex)
            return "FAILED";
        std::string out;
        for (const ShexInstruction &in : shex->instructions)
            if (!in.declaration || in.opcode == 0x71 || in.opcode == 0x72 || in.opcode == 0x73 || in.opcode == 0x74)
                out += (out.empty() ? "" : " ") + instruction_name(in);
        return out;
    }

    // The positions of the discards among the instructions, declarations left out.
    std::vector<size_t> discards(const std::vector<uint8_t> &code)
    {
        std::vector<size_t> out;
        const std::optional<Shex> shex = decode_shex(code.data(), code.size());
        size_t at = 0;
        for (const ShexInstruction &in : shex->instructions)
            if (!in.declaration)
            {
                if (in.opcode == opcode::discard)
                    out.push_back(at);
                at++;
            }
        return out;
    }

    size_t count(const std::string &names) { return names.empty() ? 0 : size_t(std::count(names.begin(), names.end(), ' ')) + 1; }

    void put32(std::vector<uint8_t> &code, size_t at, uint32_t value) { std::memcpy(code.data() + at, &value, 4); }
}

int main()
{
    const std::vector<uint8_t> instanced = load("5e9447eb5599aa41.ps.dxbc"), culled = load("a4cbd77dc355159d.ps.dxbc"), dither = load("836da523e2ec9ecb.ps.dxbc"),
        leaves = load("0a48f0bb29e3c15d.ps.dxbc"), facing = load("0b0d6421c1054820.ps.dxbc"), grass = load("aae32fcfa5c1a0bb.ps.dxbc"),
        fade = load("ee0c3fbbf7ebfae7.ps.dxbc"), character = load("41581d1e6aa3448f.ps.dxbc"), cutoff = load("68b05754fdce207c.ps.dxbc"),
        prepass = load("482d98462fb30496.ps.dxbc");
    check("5e9447eb: the per-instance cutoff decodes as vkd3d disassembles it", names(instanced) == "iadd sample add lt discard_nz mov ret");
    check("a4cbd77d: the camera-distance cull and alpha test, an if-else and a sample_l, as vkd3d's",
        names(culled)
            == "add dp3 sqrt mad lt discard_nz dp3 rsq mul sample add lt discard_nz add mad mul mul mul add dp4 rsq div mul mad dp3_sat add log mul exp "
               "mul mad mad max add mul max min mul ftou utof mul ne if_nz sample lt mul lt mul mul mov mov movc mov movc else mov endif mul dp2 frc frc "
               "sample_l mad mul movc utof mad mul ne movc mov mov mov mov mov ret");
    check("836da523: the screen-door dither's nested ifs, bfi and add_sat, as vkd3d's",
        names(dither)
            == "dp3 rsq mul div add_sat add mad add mad add_sat mad mul ge movc add lt if_nz ftoi if_z bfi bfi mad lt discard_nz else bfi bfi mad add "
               "lt discard_nz endif endif add mul mul mad mad ftou utof mul mad ne mul movc mov mad mov mov mov mov ret");
    check("the other fixtures decode to vkd3d's instruction counts, with their discards where vkd3d has them",
        count(names(leaves)) == 91 && discards(leaves) == std::vector<size_t> { 14 } && count(names(facing)) == 102
            && count(names(grass)) == 95 && count(names(fade)) == 52 && discards(fade) == std::vector<size_t> { 14, 21, 32 }
            && count(names(character)) == 473 && count(names(cutoff)) == 122 && count(names(prepass)) > 0);
    {
        const Shex shex = *decode_shex(instanced.data(), instanced.size());
        std::vector<const ShexInstruction *> code;
        for (const ShexInstruction &in : shex.instructions)
            if (!in.declaration)
                code.push_back(&in);
        const ShexInstruction &sample = *code[1], &add = *code[2], &discard = *code[4];
        check("a pixel shader of Shader Model 5.0", shex.stage == ShaderStageKind::ps && shex.major == 5 && shex.minor == 0);
        check("sample_indexable(texture2d) r0.y, v1.xyxx, t0.xwyz, s0: a 2D texture from its extended token, the mask, swizzles and slots",
            sample.opcode == opcode::sample && sample.resource_dimension == 3 && sample.operands.size() == 4 && sample.operands[0].type == operand::temp
                && sample.operands[0].mask == 2 && sample.operands[1].type == operand::input && sample.operands[1].index[0].value == 1
                && sample.operands[1].swizzle == std::array<uint8_t, 4> { 0, 1, 0, 0 } && sample.operands[2].type == operand::resource
                && sample.operands[2].swizzle == std::array<uint8_t, 4> { 0, 3, 1, 2 } && sample.operands[3].type == operand::sampler
                && sample.operands[3].index[0].value == 0);
        const ShexOperand &row = add.operands[2];
        check("add r0.x, r0.y, -cb1[r0.x + 0].x: a negated constant buffer row indexed by r0.x",
            row.type == operand::constant_buffer && row.negate && !row.absolute && row.dimension == 2 && row.index[0].value == 1 && row.index[1].relative
                && row.index[1].value == 0 && row.index[1].relative_type == operand::temp && row.index[1].relative_register == 0
                && row.index[1].relative_component == 0 && row.swizzle == std::array<uint8_t, 4> { 0, 0, 0, 0 });
        check("discard_nz r0.x: the test is nonzero", discard.opcode == opcode::discard && discard.nonzero() && instruction_name(discard) == "discard_nz");
        const auto input = std::find_if(shex.instructions.begin(), shex.instructions.end(), [](const ShexInstruction &in) {
            return in.opcode == opcode::dcl_input_ps && in.operands[0].index[0].value == 6;
        });
        check("dcl_input_ps constant v6.x: its register and constant interpolation", input != shex.instructions.end() && input->interpolation() == 1
            && input->operands[0].mask == 1);
        const std::optional<std::vector<SignatureElement>> inputs = parse_input_elements(instanced.data(), instanced.size());
        check("its 7 inputs: SV_Position, TEXCOORD0 in v1.xyz, and the vertex shader's SV_InstanceID0 in v6.x as a uint with no system value",
            inputs && inputs->size() == 7 && (*inputs)[0].system_value == 1 && (*inputs)[1].semantic == "TEXCOORD" && (*inputs)[1].index == 0
                && (*inputs)[1].reg == 1 && (*inputs)[1].mask == 7 && (*inputs)[1].component_type == 3 && (*inputs)[6].semantic == "SV_InstanceID"
                && (*inputs)[6].reg == 6 && (*inputs)[6].system_value == 0 && (*inputs)[6].component_type == 1);
    }
    {
        const Shex shex = *decode_shex(culled.data(), culled.size());
        const auto saturated = std::find_if(shex.instructions.begin(), shex.instructions.end(), [](const ShexInstruction &in) { return in.opcode == opcode::dp3 && in.saturate(); });
        check("dp3_sat: the saturate flag", saturated != shex.instructions.end() && instruction_name(*saturated) == "dp3_sat");
        const auto literal = std::find_if(shex.instructions.begin(), shex.instructions.end(), [](const ShexInstruction &in) { return in.opcode == opcode::mad; });
        check("mad r1.x, -r0.w, cb1[6].w, l(1.0): a negated temp, a selected row and a one-component literal",
            literal != shex.instructions.end() && literal->operands[1].negate && literal->operands[2].index[1].value == 6
                && literal->operands[2].swizzle == std::array<uint8_t, 4> { 3, 3, 3, 3 } && literal->operands[3].type == operand::immediate32
                && literal->operands[3].components == 1 && literal->operands[3].value[0] == 0x3F800000u);
    }
    {
        bool cut = true;
        for (size_t size = 0; size < leaves.size(); size += 4)
            cut &= !decode_shex(leaves.data(), size);
        check("0a48f0bb cut at every 4-byte boundary decodes to nothing", cut);
        bool shortened = true;
        for (size_t size = 32; size < leaves.size(); size += 4)
        {
            // The container claims the shorter size, so its chunks, not its header, run past the end.
            std::vector<uint8_t> copy(leaves.begin(), leaves.begin() + static_cast<std::ptrdiff_t>(size));
            put32(copy, 24, static_cast<uint32_t>(size));
            shortened &= !decode_shex(copy.data(), copy.size());
        }
        check("and with its header's size shortened too", shortened);
        std::vector<uint8_t> magic = leaves;
        magic[0] = 'X';
        check("a bad magic decodes to nothing", !decode_shex(magic.data(), magic.size()) && !decode_shex(nullptr, 0));
        std::vector<uint8_t> overlong = leaves;
        const size_t shex_at = [&] {
            for (size_t at = 32; at + 4 <= overlong.size(); at += 4)
                if (std::memcmp(overlong.data() + at, "SHEX", 4) == 0)
                    return at;
            return size_t(0);
        }();
        // The first instruction's length grows past the chunk.
        put32(overlong, shex_at + 16, (leaves[shex_at + 16] | leaves[shex_at + 17] << 8) | 0x7Fu << 24);
        check("an instruction longer than its chunk decodes to nothing", shex_at != 0 && !decode_shex(overlong.data(), overlong.size()));
    }
    std::printf("%s: %d failed\n", __FILE__, failures);
    return failures == 0 ? 0 : 1;
}
