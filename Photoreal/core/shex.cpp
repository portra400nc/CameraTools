#include "shex.h"

#include <cstring>

namespace photoreal
{
    namespace
    {
        constexpr size_t kHeader = 32;

        bool read32(const uint8_t *code, size_t end, size_t at, uint32_t &out)
        {
            if (at > end || end - at < 4)
                return false;
            std::memcpy(&out, code + at, 4);
            return true;
        }

        // vkd3d's names by opcode (its tpf.c maps each to an IR opcode, whose name ir.c holds), dcl_resource's spelled out.
        constexpr const char *kNames[] = {
"add", "and", "break", "breakp", nullptr, nullptr, "case", "continue", "continuec", "cut", "default", "dsx", "dsy", "discard", "div", "dp2",
        "dp3", "dp4", "else", "emit", nullptr, "endif", "endloop", "endswitch", "eq", "exp", "frc", "ftoi", "ftou", "ge", "iadd", "if", "ieq", "ige",
        "ilt", "imad", "imax", "imin", "imul", "ine", "ineg", "ishl", "ishr", "itof", "label", "ld", "ld2dms", "log", "loop", "lt", "mad", "min",
        "max", "dcl_immediateConstantBuffer", "mov", "movc", "mul", "ne", "nop", "not", "or", "resinfo", "ret", "retp", "round_ne", "round_ni",
        "round_pi", "round_z", "rsq", "sample", "sample_c", "sample_c_lz", "sample_l", "sample_d", "sample_b", "sqrt", "switch", "sincos", "udiv",
        "ult", "uge", "umul", nullptr, "umax", "umin", "ushr", "utof", "xor", "dcl_resource", "dcl_constantBuffer", "dcl_sampler", "dcl_index_range",
        "dcl_outputtopology", "dcl_inputprimitive", "dcl_maxout", "dcl_input", "dcl_input_sgv", "dcl_input_siv", "dcl_input_ps", "dcl_input_ps_sgv",
        "dcl_input_ps_siv", "dcl_output", "dcl_output_sgv", "dcl_output_siv", "dcl_temps", "dcl_indexableTemp", "dcl_globalFlags", nullptr, "lod",
        "gather4", "sample_pos", "sample_info", nullptr, "hs_decls", "hs_control_point_phase", "hs_fork_phase", "hs_join_phase", "emit_stream",
        "cut_stream", nullptr, "fcall", "bufinfo", "deriv_rtx_coarse", "deriv_rtx_fine", "deriv_rty_coarse", "deriv_rty_fine", "gather4_c",
        "gather4_po", "gather4_po_c", "rcp", "f32tof16", "f16tof32", nullptr, nullptr, "countbits", "firstbit_hi", "firstbit_lo", "firstbit_shi",
        "ubfe", "ibfe", "bfi", "bfrev", "swapc", "dcl_stream", "dcl_function_body", "dcl_function_table", "dcl_interface",
        "dcl_input_control_point_count", "dcl_output_control_point_count", "dcl_tessellator_domain", "dcl_tessellator_partitioning",
        "dcl_tessellator_output_primitive", "dcl_hs_max_tessfactor", "dcl_hs_fork_phase_instance_count", "dcl_hs_join_phase_instance_count",
        "dcl_thread_group", "dcl_uav_typed", "dcl_uav_raw", "dcl_uav_structured", "dcl_tgsm_raw", "dcl_tgsm_structured", "dcl_resource_raw",
        "dcl_resource_structured", "ld_uav_typed", "store_uav_typed", "ld_raw", "store_raw", "ld_structured", "store_structured", "atomic_and",
        "atomic_or", "atomic_xor", "atomic_cmp_store", "atomic_iadd", "atomic_imax", "atomic_imin", "atomic_umax", "atomic_umin", "imm_atomic_alloc",
        "imm_atomic_consume", "imm_atomic_iadd", "imm_atomic_and", "imm_atomic_or", "imm_atomic_xor", "imm_atomic_exch", "imm_atomic_cmp_exch",
        "imm_atomic_imax", "imm_atomic_imin", "imm_atomic_umax", "imm_atomic_umin", "sync", "dadd", "dmax", "dmin", "dmul", "deq", "dge", "dlt",
        "dne", "dmov", "dmovc", "dtof", "ftod", nullptr, "eval_sample_index", "eval_centroid", "dcl_gs_instances", nullptr, nullptr, nullptr, "ddiv",
        "dfma", "drcp", "msad", "dtoi", "dtou", "itod", "utod", nullptr, "gather4_s", "gather4_c_s", "gather4_po_s", "gather4_po_c_s", "ld_s",
        "ld2dms_s", "ld_uav_typed_s", "ld_raw_s", "ld_structured_s", "sample_l_s", "sample_c_lz_s", "sample_cl_s", "sample_b_cl_s", "sample_d_cl_s",
        "sample_c_cl_s", "check_access_fully_mapped",
        };
        static_assert(std::size(kNames) == 235);

        // Whose operands are not decoded: declarations, customdata and the hull shader's phase markers, which take no
        // operands or take tokens that are not operands.
        bool undecoded(uint32_t op)
        {
            return op == opcode::customdata || (op >= 0x58 && op <= 0x6a) || (op >= 0x71 && op <= 0x74) || (op >= 0x8f && op <= 0xa2) || op == 0xce
                || opcode_name(op) == nullptr;
        }

        // One operand at at, which moves past it. An operand's token, then its extended tokens, then an immediate's values,
        // then each index: 32 or 64 bits, a relative operand, or both. A relative operand's own index may not be relative
        // again past depth 2.
        bool read_operand(const uint8_t *code, size_t end, size_t &at, ShexOperand &out, int depth)
        {
            uint32_t token = 0;
            if (depth > 2 || !read32(code, end, at, token))
                return false;
            at += 4;
            const uint32_t count = token & 3u;
            if (count == 3)
                return false;   // N components: never in Shader Model 5 code
            out.components = count == 0 ? 0 : count == 1 ? 1 : 4;
            // A one-component operand reads its only component in every lane.
            if (out.components == 1)
                out.swizzle.fill(0);
            if (out.components == 4)
            {
                const uint32_t mode = token >> 2 & 3u;
                if (mode == 0)
                    out.mask = token >> 4 & 15u;
                else if (mode == 1)
                    for (uint32_t c = 0; c < 4; c++)
                        out.swizzle[c] = static_cast<uint8_t>(token >> (4 + 2 * c) & 3u);
                else if (mode == 2)
                    out.swizzle.fill(static_cast<uint8_t>(token >> 4 & 3u));
                else
                    return false;
            }
            out.type = static_cast<uint8_t>(token >> 12 & 0xFFu);
            out.dimension = static_cast<uint8_t>(token >> 20 & 3u);
            for (uint32_t extended = token >> 31; extended != 0;)
            {
                uint32_t more = 0;
                if (!read32(code, end, at, more))
                    return false;
                at += 4;
                // Type 1: a modifier, bits 6 to 13: 1 negate, 2 absolute, 3 both.
                if ((more & 63u) == 1)
                {
                    const uint32_t modifier = more >> 6 & 0xFFu;
                    out.negate = (modifier & 1u) != 0;
                    out.absolute = (modifier & 2u) != 0;
                }
                extended = more >> 31;
            }
            const uint32_t values = out.components == 4 ? 4 : out.components;
            if (out.type == operand::immediate32)
            {
                for (uint32_t c = 0; c < values; c++)
                {
                    if (!read32(code, end, at, out.value[c]))
                        return false;
                    at += 4;
                }
            }
            else if (out.type == operand::immediate64)
            {
                if (at > end || end - at < size_t(values) * 8)
                    return false;
                at += size_t(values) * 8;
            }
            for (uint32_t d = 0; d < out.dimension; d++)
            {
                const uint32_t representation = token >> (22 + 3 * d) & 7u;
                ShexIndex &index = out.index[d];
                if (representation == 0 || representation == 1 || representation == 3 || representation == 4)
                {
                    // 32 bits, or 64 whose low half is the value.
                    if (!read32(code, end, at, index.value))
                        return false;
                    at += representation == 1 || representation == 4 ? 8 : 4;
                    if (at > end)
                        return false;
                }
                if (representation == 2 || representation == 3 || representation == 4)
                {
                    ShexOperand relative;
                    if (!read_operand(code, end, at, relative, depth + 1))
                        return false;
                    index.relative = true;
                    index.relative_type = relative.type;
                    index.relative_register = relative.dimension > 0 ? relative.index[0].value : 0;
                    index.relative_component = relative.swizzle[0];
                }
                else if (representation > 4)
                    return false;
            }
            return true;
        }
    }

    const char *opcode_name(uint32_t op) { return op < std::size(kNames) ? kNames[op] : nullptr; }

    std::string instruction_name(const ShexInstruction &instruction)
    {
        const char *name = opcode_name(instruction.opcode);
        std::string out = name ? name : "op" + std::to_string(instruction.opcode);
        const uint32_t op = instruction.opcode;
        if (op == opcode::if_ || op == opcode::discard || op == opcode::breakc || op == opcode::continuec || op == opcode::retc || op == opcode::callc)
            out += instruction.nonzero() ? "_nz" : "_z";
        if (instruction.saturate())
            out += "_sat";
        return out;
    }

    std::optional<Shex> decode_shex(const uint8_t *code, size_t size)
    {
        uint32_t total = 0, count = 0;
        if (code == nullptr || size < kHeader || std::memcmp(code, "DXBC", 4) != 0 || !read32(code, size, 24, total) || total > size
            || total < kHeader || !read32(code, size, 28, count) || count > (total - kHeader) / 4)
            return std::nullopt;
        size = total;
        for (uint32_t i = 0; i < count; i++)
        {
            uint32_t offset = 0, chunk_size = 0;
            if (!read32(code, size, kHeader + 4 * i, offset) || !read32(code, size, size_t(offset) + 4, chunk_size)
                || size_t(offset) + 8 + chunk_size > size)
                return std::nullopt;
            if (std::memcmp(code + offset, "SHEX", 4) != 0 && std::memcmp(code + offset, "SHDR", 4) != 0)
                continue;
            const size_t body = size_t(offset) + 8, chunk_end = body + chunk_size;
            uint32_t version = 0, tokens = 0;
            if (!read32(code, chunk_end, body, version) || !read32(code, chunk_end, body + 4, tokens) || tokens < 2 || tokens > chunk_size / 4
                || version >> 16 > static_cast<uint32_t>(ShaderStageKind::cs))
                return std::nullopt;
            Shex shex;
            shex.stage = static_cast<ShaderStageKind>(version >> 16);
            shex.major = version >> 4 & 15u;
            shex.minor = version & 15u;
            const size_t end = body + size_t(tokens) * 4;
            for (size_t at = body + 8; at < end;)
            {
                uint32_t token = 0, length = 0;
                if (!read32(code, end, at, token))
                    return std::nullopt;
                ShexInstruction instruction;
                instruction.opcode = token & 0x7FFu;
                instruction.controls = token >> 11 & 0x1FFFu;
                if (instruction.opcode == opcode::customdata)
                {
                    if (!read32(code, end, at + 4, length))
                        return std::nullopt;
                }
                else
                    length = token >> 24 & 0x7Fu;
                if (length == 0 || size_t(length) * 4 > end - at)
                    return std::nullopt;
                const size_t next = at + size_t(length) * 4;
                size_t p = at + 4;
                instruction.declaration = undecoded(instruction.opcode);
                if (instruction.opcode != opcode::customdata)
                    for (uint32_t extended = token >> 31; extended != 0;)
                    {
                        uint32_t more = 0;
                        if (!read32(code, next, p, more))
                            return std::nullopt;
                        p += 4;
                        // Type 2: the resource's dimension, bits 6 to 10.
                        if ((more & 63u) == 2)
                            instruction.resource_dimension = static_cast<uint8_t>(more >> 6 & 31u);
                        extended = more >> 31;
                    }
                const bool first_only = instruction.opcode == opcode::dcl_resource || instruction.opcode == opcode::dcl_input_ps
                    || instruction.opcode == opcode::dcl_input_ps_sgv || instruction.opcode == opcode::dcl_input_ps_siv;
                if (!instruction.declaration || first_only)
                    while (p < next)
                    {
                        ShexOperand o;
                        if (!read_operand(code, next, p, o, 0))
                            return std::nullopt;
                        instruction.operands.push_back(o);
                        if (first_only)
                            break;
                    }
                shex.instructions.push_back(std::move(instruction));
                at = next;
            }
            return shex;
        }
        return std::nullopt;
    }
}
