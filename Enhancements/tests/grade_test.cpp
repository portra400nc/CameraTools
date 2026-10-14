// The grade layout finder on the tone map's code as the census shaders have it, with other rows, without a bloom, and on
// code that is not a tone map. check_grade.sh runs it over every census pixel shader.
#include "../core/grade.h"

#include <cstdio>

using namespace enhancements;

namespace
{
    int failures = 0;

    void check(const char *name, bool ok)
    {
        std::printf("%s %s\n", ok ? "PASS" : "FAIL", name);
        failures += ok ? 0 : 1;
    }

    // r<number> with a destination's mask, or a source's swizzle.
    ShexOperand temp(uint32_t number, uint8_t mask)
    {
        ShexOperand o;
        o.type = operand::temp;
        o.components = 4;
        o.mask = mask;
        o.dimension = 1;
        o.index[0].value = number;
        return o;
    }

    ShexOperand temp(uint32_t number, std::array<uint8_t, 4> swizzle)
    {
        ShexOperand o = temp(number, uint8_t { 0 });
        o.swizzle = swizzle;
        return o;
    }

    ShexOperand cb(uint32_t row, std::array<uint8_t, 4> swizzle, uint32_t slot = 0)
    {
        ShexOperand o;
        o.type = operand::constant_buffer;
        o.components = 4;
        o.swizzle = swizzle;
        o.dimension = 2;
        o.index[0].value = slot;
        o.index[1].value = row;
        return o;
    }

    ShexOperand literal()
    {
        ShexOperand o;
        o.type = operand::immediate32;
        o.components = 4;
        return o;
    }

    ShexInstruction op(uint32_t opcode, std::vector<ShexOperand> operands)
    {
        ShexInstruction in;
        in.opcode = opcode;
        in.operands = std::move(operands);
        return in;
    }

    constexpr uint8_t kXyz = 7, kYzw = 14;
    constexpr std::array<uint8_t, 4> kX = { 0, 0, 0, 0 }, kY = { 1, 1, 1, 1 }, kZ = { 2, 2, 2, 2 }, kXyzx = { 0, 1, 2, 0 }, kXxyz = { 0, 0, 1, 2 },
                                     kYzwy = { 1, 2, 3, 1 }, kYyzw = { 1, 1, 2, 3 };

    // The census tone map's instructions that carry the grade, with the matrix at `color` and the scalars where given.
    Shex tonemap(uint32_t color, bool bloom, uint32_t green_row_offset = 1)
    {
        Shex shex;
        std::vector<ShexInstruction> &code = shex.instructions;
        code.push_back(op(opcode::sample, { temp(0, kXyz), temp(9, kXyzx), literal(), literal() }));
        if (bloom)
            code.push_back(op(opcode::mad, { temp(0, kXyz), temp(1, kXyzx), cb(22, kY), temp(0, kXyzx) }));
        // Eye adaptation: a float of b0 times a temp plus a literal, which is not the bloom.
        code.push_back(op(opcode::mad, { temp(1, uint8_t { 1 }), cb(98, kX), temp(1, kX), literal() }));
        code.push_back(op(opcode::mul, { temp(1, kYzw), temp(0, kY), cb(color + green_row_offset, kXxyz) }));
        code.push_back(op(opcode::mad, { temp(1, kYzw), cb(color, kXxyz), temp(0, kX), temp(1, kYyzw) }));
        code.push_back(op(opcode::mad, { temp(1, kYzw), cb(color + 2, kXxyz), temp(0, kZ), temp(1, kYyzw) }));
        code.push_back(op(opcode::mul, { temp(2, kYzw), temp(1, kYyzw), temp(2, kY) }));
        code.push_back(op(opcode::mul, { temp(3, kXyz), temp(1, kYzwy), cb(22, kZ) }));
        // The encode's own power is by a literal.
        code.push_back(op(opcode::log, { temp(1, kYzw), temp(1, kYyzw) }));
        code.push_back(op(opcode::mul, { temp(1, kYzw), temp(1, kYyzw), literal() }));
        code.push_back(op(opcode::exp, { temp(1, kYzw), temp(1, kYyzw) }));
        code.push_back(op(opcode::log, { temp(1, kYzw), temp(1, kYyzw) }));
        code.push_back(op(opcode::mul, { temp(1, kYzw), temp(1, kYyzw), cb(20, kY) }));
        code.push_back(op(opcode::exp, { temp(1, kYzw), temp(1, kYyzw) }));
        return shex;
    }

    bool at(const GradeValue &value, uint32_t row, uint8_t component) { return value.row == row && value.component == component; }
}

int main()
{
    {
        const std::optional<GradeLayout> layout = find_grade(tonemap(99, true));
        check("the census tone map: the matrix in rows 99 to 101", layout && layout->color_row == 99);
        check("the census tone map: exposure 22.z, gamma 20.y, bloom 22.y",
            layout && at(layout->exposure, 22, 2) && at(layout->gamma, 20, 1) && layout->bloom && at(*layout->bloom, 22, 1));
        check("the census tone map reaches 102 rows of b0", layout && layout->bytes() == 102 * 16);
        check("exposure 22.z is byte 360 of b0", layout && layout->exposure.offset() == 360);
    }
    {
        const std::optional<GradeLayout> layout = find_grade(tonemap(91, true));
        check("a variant with the matrix eight rows earlier is found there", layout && layout->color_row == 91 && at(layout->exposure, 22, 2));
    }
    {
        const std::optional<GradeLayout> layout = find_grade(tonemap(99, false));
        check("a variant that adds no bloom has no bloom and keeps the rest", layout && !layout->bloom && layout->color_row == 99 && at(layout->gamma, 20, 1));
    }
    check("rows that are not a run are no color matrix", !find_grade(tonemap(99, true, 3)));
    {
        Shex shex = tonemap(99, true);
        shex.instructions.erase(shex.instructions.begin() + 7);
        check("a matrix whose result is never scaled by a float of b0 is no tone map", !find_grade(shex));
    }
    {
        Shex shex = tonemap(99, true);
        shex.instructions.resize(shex.instructions.size() - 3);
        check("without a power by a float of b0 there is no layout", !find_grade(shex));
    }
    {
        Shex shex = tonemap(99, true);
        for (ShexInstruction &in : shex.instructions)
            for (ShexOperand &o : in.operands)
                if (o.type == operand::constant_buffer)
                    o.index[0].value = 1;
        check("rows of another constant buffer are not the game's b0", !find_grade(shex));
    }
    check("an empty shader has no layout", !find_grade(Shex {}));
    return failures == 0 ? 0 : 1;
}
