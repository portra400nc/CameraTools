#include "grade.h"

#include <algorithm>

namespace enhancements
{
    namespace
    {
        // How far past the color matrix the exposure's multiply may sit.
        constexpr size_t kExposureReach = 12;

        bool is_temp(const ShexOperand &o) { return o.type == operand::temp && o.dimension == 1 && !o.index[0].relative; }

        // The row of b0 a constant buffer operand reads, when the row is fixed.
        std::optional<uint32_t> row_of(const ShexOperand &o)
        {
            if (o.type != operand::constant_buffer || o.dimension != 2 || o.index[0].relative || o.index[1].relative || o.index[0].value != 0)
                return std::nullopt;
            return o.index[1].value;
        }

        // A destination that is three components of a temp.
        bool three_wide(const ShexOperand &o)
        {
            return is_temp(o) && o.components == 4 && (o.mask == 7 || o.mask == 11 || o.mask == 13 || o.mask == 14);
        }

        // The one component a source reads for every component the destination writes, or none when it reads several.
        std::optional<uint8_t> replicated(const ShexOperand &source, uint8_t mask)
        {
            std::optional<uint8_t> component;
            for (uint8_t c = 0; c < 4; c++)
            {
                if (!(mask >> c & 1u))
                    continue;
                if (component && *component != source.swizzle[c])
                    return std::nullopt;
                component = source.swizzle[c];
            }
            return component;
        }

        // Whether a source reads x, y and z in that order into the destination's three components.
        bool reads_xyz(const ShexOperand &source, uint8_t mask)
        {
            uint8_t next = 0;
            for (uint8_t c = 0; c < 4; c++)
                if ((mask >> c & 1u) && source.swizzle[c] != next++)
                    return false;
            return true;
        }

        // One float of b0 among an instruction's two multiplicands, with the other multiplicand.
        struct Scaled
        {
            GradeValue by;
            const ShexOperand *other = nullptr;
        };

        // operands 1 and 2 of a mul or a mad are what it multiplies.
        std::optional<Scaled> scaled(const ShexInstruction &in)
        {
            if (in.operands.size() < 3 || !three_wide(in.operands[0]))
                return std::nullopt;
            for (int i = 1; i <= 2; i++)
            {
                const std::optional<uint32_t> row = row_of(in.operands[i]);
                const std::optional<uint8_t> component = row ? replicated(in.operands[i], in.operands[0].mask) : std::nullopt;
                if (component)
                    return Scaled { { *row, *component }, &in.operands[3 - i] };
            }
            return std::nullopt;
        }

        // One term of the color matrix: a row of b0 times one channel of a temp.
        struct Term
        {
            uint32_t row = 0, from = 0;
            uint8_t channel = 0;
        };

        std::optional<Term> term(const ShexInstruction &in)
        {
            if (in.operands.size() < 3 || !three_wide(in.operands[0]))
                return std::nullopt;
            const uint8_t mask = in.operands[0].mask;
            for (int i = 1; i <= 2; i++)
            {
                const ShexOperand &rows = in.operands[i], &channel = in.operands[3 - i];
                const std::optional<uint32_t> row = row_of(rows);
                if (!row || !reads_xyz(rows, mask) || !is_temp(channel))
                    continue;
                if (const std::optional<uint8_t> component = replicated(channel, mask))
                    return Term { *row, channel.index[0].value, *component };
            }
            return std::nullopt;
        }

        struct Matrix
        {
            uint32_t row = 0, result = 0;   // the first row, and the temp the product lands in
        };

        // A mul and two mads into one temp, whose rows of b0 run from red's to blue's.
        std::optional<Matrix> matrix_at(const std::vector<ShexInstruction> &code, size_t at)
        {
            if (at + 3 > code.size() || code[at].opcode != opcode::mul || code[at + 1].opcode != opcode::mad || code[at + 2].opcode != opcode::mad)
                return std::nullopt;
            uint32_t rows[3] = {};
            uint8_t seen = 0;
            std::optional<uint32_t> from;
            const ShexOperand &result = code[at].operands.empty() ? ShexOperand {} : code[at].operands[0];
            for (size_t i = at; i < at + 3; i++)
            {
                const std::optional<Term> t = term(code[i]);
                if (!t || t->channel > 2 || (from && *from != t->from) || code[i].operands[0].index[0].value != result.index[0].value
                    || code[i].operands[0].mask != result.mask)
                    return std::nullopt;
                from = t->from;
                rows[t->channel] = t->row;
                seen |= static_cast<uint8_t>(1u << t->channel);
            }
            if (seen != 7 || rows[1] != rows[0] + 1 || rows[2] != rows[0] + 2)
                return std::nullopt;
            return Matrix { rows[0], result.index[0].value };
        }
    }

    uint32_t GradeLayout::bytes() const
    {
        uint32_t last = std::max({ color_row + 2, exposure.row, gamma.row });
        if (bloom)
            last = std::max(last, bloom->row);
        return (last + 1) * 16;
    }

    std::optional<GradeLayout> find_grade(const Shex &shader)
    {
        const std::vector<ShexInstruction> &code = shader.instructions;
        std::optional<Matrix> matrix;
        size_t at = 0;
        for (; at < code.size() && !matrix; at++)
            matrix = matrix_at(code, at);
        if (!matrix)
            return std::nullopt;
        at--;
        GradeLayout layout;
        layout.color_row = matrix->row;

        // The bloom is added to the scene before the matrix: a temp times one float of b0, plus a temp.
        for (size_t i = 0; i < at && !layout.bloom; i++)
        {
            if (code[i].opcode != opcode::mad || code[i].operands.size() != 4 || !is_temp(code[i].operands[3]))
                continue;
            if (const std::optional<Scaled> s = scaled(code[i]); s && is_temp(*s->other))
                layout.bloom = s->by;
        }

        // The exposure comes before the curve and the encode, whose powers are the first log after the matrix.
        bool exposed = false;
        for (size_t i = at + 3; i < code.size() && i < at + 3 + kExposureReach && code[i].opcode != opcode::log && !exposed; i++)
        {
            if (code[i].opcode != opcode::mul)
                continue;
            if (const std::optional<Scaled> s = scaled(code[i]); s && is_temp(*s->other) && s->other->index[0].value == matrix->result)
            {
                layout.exposure = s->by;
                exposed = true;
            }
        }
        if (!exposed)
            return std::nullopt;

        for (size_t i = at + 3; i + 2 < code.size(); i++)
        {
            if (code[i].opcode != opcode::log || code[i + 1].opcode != opcode::mul || code[i + 2].opcode != opcode::exp)
                continue;
            if (const std::optional<Scaled> s = scaled(code[i + 1]))
            {
                layout.gamma = s->by;
                return layout;
            }
        }
        return std::nullopt;
    }
}
