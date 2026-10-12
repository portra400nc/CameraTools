#include "alpha.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <set>
#include <stdexcept>

namespace photoreal
{
    const std::array<const char *, static_cast<size_t>(DiscardKind::count)> kDiscardKindNames = { "texture", "dither", "other", "unsupported" };

    namespace
    {
        constexpr uint32_t kPosition = 1;       // D3D_NAME_POSITION
        constexpr uint32_t kFloat32 = 3;        // D3D_REGISTER_COMPONENT_FLOAT32
        constexpr uint32_t kConstant = 1;       // D3D10_SB_INTERPOLATION_CONSTANT
        constexpr uint32_t kTexture2d = 3;      // D3D10_SB_RESOURCE_DIMENSION_TEXTURE2D
        constexpr uint32_t kInputRegister = kAlphaTemps;

        // How an instruction's sources map to its result.
        enum class Shape : uint8_t { lanes, dot, sample };

        struct Operation
        {
            uint32_t op;        // ours, alpha_op
            Shape shape;
            uint8_t dot;        // components a dot product reads
        };

        // The game's opcodes the program runs, and as which of ours. Everything else in a slice makes it unsupported.
        std::optional<Operation> operation_of(uint32_t sm)
        {
            using namespace alpha_op;
            switch (sm)
            {
            case opcode::mov: return Operation { mov, Shape::lanes, 0 };
            case opcode::movc: return Operation { movc, Shape::lanes, 0 };
            case opcode::add: return Operation { add, Shape::lanes, 0 };
            case opcode::mul: return Operation { mul, Shape::lanes, 0 };
            case opcode::mad: return Operation { mad, Shape::lanes, 0 };
            case opcode::div: return Operation { div, Shape::lanes, 0 };
            case opcode::min: return Operation { min, Shape::lanes, 0 };
            case opcode::max: return Operation { max, Shape::lanes, 0 };
            case opcode::dp2: return Operation { dp2, Shape::dot, 2 };
            case opcode::dp3: return Operation { dp3, Shape::dot, 3 };
            case opcode::dp4: return Operation { dp4, Shape::dot, 4 };
            case opcode::lt: return Operation { lt, Shape::lanes, 0 };
            case opcode::ge: return Operation { ge, Shape::lanes, 0 };
            case opcode::eq: return Operation { eq, Shape::lanes, 0 };
            case opcode::ne: return Operation { ne, Shape::lanes, 0 };
            case opcode::ftoi: return Operation { ftoi, Shape::lanes, 0 };
            case opcode::ftou: return Operation { ftou, Shape::lanes, 0 };
            case opcode::round_ne: return Operation { round_ne, Shape::lanes, 0 };
            case opcode::round_ni: return Operation { round_ni, Shape::lanes, 0 };
            case opcode::round_pi: return Operation { round_pi, Shape::lanes, 0 };
            case opcode::round_z: return Operation { round_z, Shape::lanes, 0 };
            case opcode::frc: return Operation { frc, Shape::lanes, 0 };
            case opcode::rsq: return Operation { rsq, Shape::lanes, 0 };
            case opcode::sqrt: return Operation { sqrt, Shape::lanes, 0 };
            case opcode::exp: return Operation { exp, Shape::lanes, 0 };
            case opcode::log: return Operation { log, Shape::lanes, 0 };
            case opcode::rcp: return Operation { rcp, Shape::lanes, 0 };
            case opcode::sample: case opcode::sample_l: case opcode::sample_b: case opcode::sample_d: return Operation { sample, Shape::sample, 0 };
            case opcode::and_: return Operation { and_, Shape::lanes, 0 };
            case opcode::or_: return Operation { or_, Shape::lanes, 0 };
            case opcode::xor_: return Operation { xor_, Shape::lanes, 0 };
            case opcode::not_: return Operation { not_, Shape::lanes, 0 };
            case opcode::iadd: return Operation { iadd, Shape::lanes, 0 };
            case opcode::ieq: return Operation { ieq, Shape::lanes, 0 };
            case opcode::ine: return Operation { ine, Shape::lanes, 0 };
            case opcode::ilt: return Operation { ilt, Shape::lanes, 0 };
            case opcode::ige: return Operation { ige, Shape::lanes, 0 };
            case opcode::ult: return Operation { ult, Shape::lanes, 0 };
            case opcode::uge: return Operation { uge, Shape::lanes, 0 };
            case opcode::imin: return Operation { imin, Shape::lanes, 0 };
            case opcode::imax: return Operation { imax, Shape::lanes, 0 };
            case opcode::umin: return Operation { umin, Shape::lanes, 0 };
            case opcode::umax: return Operation { umax, Shape::lanes, 0 };
            case opcode::ineg: return Operation { ineg, Shape::lanes, 0 };
            case opcode::ishl: return Operation { ishl, Shape::lanes, 0 };
            case opcode::ishr: return Operation { ishr, Shape::lanes, 0 };
            case opcode::ushr: return Operation { ushr, Shape::lanes, 0 };
            case opcode::itof: return Operation { itof, Shape::lanes, 0 };
            case opcode::utof: return Operation { utof, Shape::lanes, 0 };
            default: return std::nullopt;
            }
        }

        // Control flow a slice cannot follow: loops, switches, calls and early returns.
        bool unstructured(uint32_t op)
        {
            return op == opcode::loop || op == opcode::endloop || op == opcode::switch_ || op == opcode::case_ || op == opcode::default_
                || op == opcode::endswitch || op == opcode::break_ || op == opcode::breakc || op == opcode::continue_ || op == opcode::continuec
                || op == opcode::call || op == opcode::callc || op == opcode::ret || op == opcode::retc || op == opcode::label;
        }

        // Opcodes whose first two operands are destinations.
        bool two_destinations(uint32_t op) { return op == 0x26 || op == 0x4d || op == 0x4e || op == 0x51 || op == 0x84 || op == 0x85 || op == 0x8e; }

        uint32_t first_component(uint8_t mask)
        {
            for (uint32_t c = 0; c < 4; c++)
                if (mask >> c & 1u)
                    return c;
            return 0;
        }

        // The shader's code without its declarations, with what the slices need of the declarations and the signature.
        struct Code
        {
            std::vector<const ShexInstruction *> at;    // the instructions, declarations left out
            std::vector<int> owner;                     // an else's or endif's if; -1 elsewhere
            std::vector<int> end;                       // an if's endif
            std::vector<uint32_t> depth;                // ifs around each instruction
            std::map<uint32_t, uint32_t> interpolation; // by input register, from dcl_input_ps
            std::map<uint32_t, uint32_t> dimension;     // by texture slot, from dcl_resource
            std::vector<SignatureElement> inputs;
            bool balanced = true;                       // every if has its endif

            const SignatureElement *element(uint32_t reg, uint32_t component) const
            {
                for (const SignatureElement &e : inputs)
                    if (e.reg == reg && (e.mask >> component & 1u))
                        return &e;
                return nullptr;
            }
        };

        Code index_code(const Shex &shex, std::vector<SignatureElement> inputs)
        {
            Code code;
            code.inputs = std::move(inputs);
            std::vector<int> ifs;
            for (const ShexInstruction &in : shex.instructions)
            {
                if (in.declaration)
                {
                    if ((in.opcode == opcode::dcl_input_ps || in.opcode == opcode::dcl_input_ps_siv || in.opcode == opcode::dcl_input_ps_sgv)
                        && !in.operands.empty() && in.operands[0].dimension > 0)
                        code.interpolation[in.operands[0].index[0].value] = in.interpolation();
                    if (in.opcode == opcode::dcl_resource && !in.operands.empty() && in.operands[0].dimension > 0)
                        code.dimension[in.operands[0].index[0].value] = in.declared_dimension();
                    continue;
                }
                const int position = static_cast<int>(code.at.size());
                code.at.push_back(&in);
                code.owner.push_back(-1);
                code.end.push_back(-1);
                code.depth.push_back(static_cast<uint32_t>(ifs.size()));
                if (in.opcode == opcode::if_)
                    ifs.push_back(position);
                else if (in.opcode == opcode::else_ || in.opcode == opcode::endif)
                {
                    if (ifs.empty())
                    {
                        code.balanced = false;
                        continue;
                    }
                    code.owner[position] = ifs.back();
                    code.depth[position] = static_cast<uint32_t>(ifs.size() - 1);
                    if (in.opcode == opcode::endif)
                    {
                        code.end[ifs.back()] = position;
                        ifs.pop_back();
                    }
                }
            }
            code.balanced &= ifs.empty();
            return code;
        }

        // What a backward slice from some discards found.
        struct Slice
        {
            std::vector<bool> emitted;          // the instructions the slice runs, ifs and discards included
            std::vector<uint8_t> mask;          // each emitted instruction's destination components the slice reads
            std::set<std::pair<uint32_t, uint32_t>> inputs;     // input register and component
            std::map<uint32_t, std::set<uint32_t>> rows;        // constant buffer slot: its rows read directly
            std::set<uint32_t> dynamic;         // constant buffer slots read with a register's value as the index
            std::set<std::pair<uint32_t, uint32_t>> textures;   // texture slot and channel
            std::set<uint32_t> samplers;
            std::set<std::array<uint32_t, 4>> literals;
            bool dither = false;
            bool samples = false;
            std::string unsupported;            // the first reason
        };

        void refuse(Slice &s, const std::string &why)
        {
            if (s.unsupported.empty())
                s.unsupported = why;
        }

        // A source's components in the given lanes: registers become live, the rest leaves of the slice.
        void read(const Code &code, Slice &s, std::set<uint32_t> &live, const ShexOperand &o, uint32_t lanes)
        {
            const auto component = [&](uint32_t lane) { return uint32_t(o.swizzle[lane]); };
            switch (o.type)
            {
            case operand::temp:
                for (uint32_t c = 0; c < 4; c++)
                    if (lanes >> c & 1u)
                        live.insert(o.index[0].value * 4 + component(c));
                break;
            case operand::input:
                for (uint32_t c = 0; c < 4; c++)
                    if (lanes >> c & 1u)
                    {
                        const SignatureElement *e = code.element(o.index[0].value, component(c));
                        if (e && e->system_value == kPosition)
                            s.dither = true;
                        else if (!e)
                            refuse(s, "an input without a signature element");
                        else if (e->system_value != 0)
                            refuse(s, "input " + e->semantic);
                        else
                            s.inputs.insert({ o.index[0].value, component(c) });
                    }
                break;
            case operand::constant_buffer:
            {
                if (o.dimension < 2)
                {
                    refuse(s, "a constant buffer without a row");
                    break;
                }
                const ShexIndex &row = o.index[1];
                if (!row.relative)
                {
                    s.rows[o.index[0].value].insert(row.value);
                    break;
                }
                s.dynamic.insert(o.index[0].value);
                if (row.relative_type == operand::temp)
                    live.insert(row.relative_register * 4 + row.relative_component);
                else if (row.relative_type == operand::input)
                {
                    ShexOperand index;
                    index.type = operand::input;
                    index.dimension = 1;
                    index.index[0].value = row.relative_register;
                    index.swizzle.fill(row.relative_component);
                    read(code, s, live, index, 1);
                }
                else
                    refuse(s, "a constant buffer row indexed by an indexable temp");
                break;
            }
            case operand::immediate32:
                s.literals.insert(o.value);
                break;
            case operand::immediate_constant_buffer:
                s.dither = true;
                break;
            default:
                refuse(s, "operand type " + std::to_string(o.type));
                break;
            }
        }

        // Every instruction the discards at seeds depend on, through registers and enclosing ifs, walking back from the last.
        // A write inside an if does not end a register's liveness, since the other branch keeps its old value.
        Slice slice(const Code &code, const std::vector<int> &seeds)
        {
            Slice s;
            const int count = static_cast<int>(code.at.size());
            s.emitted.assign(code.at.size(), false);
            s.mask.assign(code.at.size(), 0);
            std::set<uint32_t> live;
            const int last = seeds.empty() ? -1 : *std::max_element(seeds.begin(), seeds.end());
            for (int i = last; i >= 0 && i < count; i--)
            {
                const ShexInstruction &in = *code.at[i];
                if (in.opcode == opcode::discard)
                {
                    if (std::find(seeds.begin(), seeds.end(), i) == seeds.end() || in.operands.empty())
                        continue;
                    s.emitted[i] = true;
                    read(code, s, live, in.operands[0], 1);
                    continue;
                }
                if (in.opcode == opcode::if_)
                {
                    const int end = code.end[i] < 0 ? count : code.end[i];
                    if (std::any_of(s.emitted.begin() + i + 1, s.emitted.begin() + end, [](bool e) { return e; }) && !in.operands.empty())
                    {
                        s.emitted[i] = true;
                        read(code, s, live, in.operands[0], 1);
                    }
                    continue;
                }
                if (in.opcode == opcode::else_ || in.opcode == opcode::endif || unstructured(in.opcode) || in.operands.empty())
                    continue;
                const int destinations = two_destinations(in.opcode) ? 2 : 1;
                uint32_t needed = 0;
                for (int d = 0; d < destinations && d < int(in.operands.size()); d++)
                {
                    const ShexOperand &dst = in.operands[d];
                    if (dst.type != operand::temp)
                        continue;
                    for (uint32_t c = 0; c < 4; c++)
                        if ((dst.mask >> c & 1u) && live.count(dst.index[0].value * 4 + c))
                            needed |= 1u << c;
                }
                if (needed == 0)
                    continue;
                s.emitted[i] = true;
                const std::optional<Operation> operation = operation_of(in.opcode);
                if (!operation || destinations == 2)
                {
                    refuse(s, std::string("opcode ") + (opcode_name(in.opcode) ? opcode_name(in.opcode) : std::to_string(in.opcode).c_str()));
                    continue;
                }
                s.mask[i] = static_cast<uint8_t>(needed);
                const uint32_t reg = in.operands[0].index[0].value;
                if (code.depth[i] == 0)
                    for (uint32_t c = 0; c < 4; c++)
                        if (needed >> c & 1u)
                            live.erase(reg * 4 + c);
                if (operation->shape == Shape::sample)
                {
                    s.samples = true;
                    if (in.operands.size() < 4 || in.operands[2].type != operand::resource || in.operands[3].type != operand::sampler)
                    {
                        refuse(s, "a sample without a texture or sampler");
                        continue;
                    }
                    const uint32_t slot = in.operands[2].index[0].value;
                    const auto declared = code.dimension.find(slot);
                    const uint32_t dimension = in.resource_dimension != 0 ? in.resource_dimension : declared == code.dimension.end() ? 0 : declared->second;
                    if (dimension != kTexture2d)
                        refuse(s, "a texture that is not 2D");
                    for (uint32_t c = 0; c < 4; c++)
                        if (needed >> c & 1u)
                            s.textures.insert({ slot, in.operands[2].swizzle[c] });
                    s.samplers.insert(in.operands[3].index[0].value);
                    read(code, s, live, in.operands[1], 3);
                    continue;
                }
                for (size_t k = 1; k < in.operands.size(); k++)
                    read(code, s, live, in.operands[k], operation->shape == Shape::dot ? (1u << operation->dot) - 1 : needed);
            }
            return s;
        }

        // Where the slice's registers and leaves go in the program.
        struct Layout
        {
            std::map<uint32_t, uint32_t> temps;                                 // the shader's temp: ours
            std::map<std::pair<uint32_t, uint32_t>, uint32_t> inputs;           // input register and component: our input float
            std::map<uint32_t, uint32_t> input_first;                           // input register: its first float
            std::map<uint32_t, uint32_t> row_base;                              // a static slot: where its rows start in the draw's
            std::map<uint32_t, uint32_t> row_first;                             // a static slot: its first row
            uint32_t dynamic_slot = kNoSlot, dynamic_base = 0;
            std::vector<std::array<uint32_t, 4>> literals;
        };

        uint32_t swizzle_bits(const std::array<uint8_t, 4> &s) { return s[0] | s[1] << 2 | s[2] << 4 | s[3] << 6; }

        uint32_t source(const Layout &layout, const ShexOperand &o)
        {
            uint32_t kind = 0, index = 0, relative = 0, relative_component = 0;
            std::array<uint8_t, 4> swizzle = o.swizzle;
            switch (o.type)
            {
            case operand::temp:
                index = layout.temps.at(o.index[0].value);
                break;
            case operand::input:
            {
                // Every component the code reads from one input register sits in one register of ours; a lane the slice
                // does not read takes its first.
                const uint32_t reg = o.index[0].value;
                const uint32_t first = layout.input_first.at(reg);
                index = kInputRegister + first / 4;
                for (uint32_t c = 0; c < 4; c++)
                {
                    const auto it = layout.inputs.find({ reg, swizzle[c] });
                    swizzle[c] = static_cast<uint8_t>((it == layout.inputs.end() ? first : it->second) % 4);
                }
                break;
            }
            case operand::constant_buffer:
            {
                const uint32_t slot = o.index[0].value;
                const ShexIndex &row = o.index[1];
                if (slot == layout.dynamic_slot)
                {
                    index = layout.dynamic_base + row.value;
                    if (row.relative)
                    {
                        kind = 2;
                        if (row.relative_type == operand::temp)
                        {
                            relative = layout.temps.at(row.relative_register);
                            relative_component = row.relative_component;
                        }
                        else
                        {
                            const uint32_t at = layout.inputs.at({ row.relative_register, row.relative_component });
                            relative = kInputRegister + at / 4;
                            relative_component = at % 4;
                        }
                    }
                    else
                        kind = 1;
                }
                else
                {
                    kind = 1;
                    index = layout.row_base.at(slot) + row.value - layout.row_first.at(slot);
                }
                break;
            }
            case operand::immediate32:
                kind = 3;
                index = static_cast<uint32_t>(std::find(layout.literals.begin(), layout.literals.end(), o.value) - layout.literals.begin());
                break;
            default:
                break;
            }
            return kind | swizzle_bits(swizzle) << 2 | uint32_t(o.negate) << 10 | uint32_t(o.absolute) << 11 | relative << 12 | relative_component << 16
                | index << 18;
        }

        std::array<uint32_t, 4> instruction(uint32_t op, uint32_t dst, uint32_t mask, bool saturate, uint32_t target)
        {
            return { op | dst << 8 | mask << 12 | uint32_t(saturate) << 16 | target << 20, 0, 0, 0 };
        }

        // The program of a slice whose every part is supported, or why there is none.
        std::optional<AlphaProgram> build(const Code &code, const Slice &s, std::string &why)
        {
            AlphaProgram p;
            Layout layout;
            std::set<std::pair<uint32_t, uint32_t>> textures = s.textures;
            if (textures.size() > 1)
            {
                why = "two textures or channels";
                return std::nullopt;
            }
            if (!textures.empty())
            {
                p.texture = static_cast<uint8_t>(textures.begin()->first);
                p.channel = static_cast<uint8_t>(textures.begin()->second);
                p.sampler = static_cast<uint8_t>(*s.samplers.begin());
            }
            // Temporaries in order of first use.
            for (size_t i = 0; i < code.at.size(); i++)
            {
                if (!s.emitted[i])
                    continue;
                for (const ShexOperand &o : code.at[i]->operands)
                {
                    if (o.type == operand::temp)
                        layout.temps.emplace(o.index[0].value, static_cast<uint32_t>(layout.temps.size()));
                    for (uint32_t d = 0; d < o.dimension; d++)
                        if (o.index[d].relative && o.index[d].relative_type == operand::temp)
                            layout.temps.emplace(o.index[d].relative_register, static_cast<uint32_t>(layout.temps.size()));
                }
            }
            if (layout.temps.size() > kAlphaTemps)
            {
                why = "more than 8 temporaries";
                return std::nullopt;
            }
            // Inputs: each input register's components together in one register of ours, and the stream-output elements
            // they come from, each from its first read component to its last.
            std::map<uint32_t, std::vector<uint32_t>> by_register;
            for (const auto &[reg, component] : s.inputs)
                by_register[reg].push_back(component);
            std::array<uint32_t, 3> used {};
            struct Element
            {
                const SignatureElement *element;
                uint32_t low = 4, high = 0;
            };
            std::vector<Element> elements;
            for (const auto &[reg, components] : by_register)
            {
                uint32_t slot = 0;
                while (slot < used.size() && used[slot] + components.size() > 4)
                    slot++;
                if (slot == used.size())
                {
                    why = "more inputs than 12 floats";
                    return std::nullopt;
                }
                layout.input_first[reg] = slot * 4 + used[slot];
                for (uint32_t c : components)
                {
                    layout.inputs[{ reg, c }] = slot * 4 + used[slot]++;
                    const SignatureElement *e = code.element(reg, c);
                    auto it = std::find_if(elements.begin(), elements.end(), [e](const Element &x) { return x.element == e; });
                    if (it == elements.end())
                        it = elements.insert(elements.end(), Element { e });
                    const uint32_t relative = c - first_component(e->mask);
                    it->low = std::min(it->low, relative);
                    it->high = std::max(it->high, relative);
                }
            }
            p.input_map.fill(0xFF);
            uint32_t floats = 0;
            for (const Element &e : elements)
            {
                const auto interpolation = code.interpolation.find(e.element->reg);
                const bool flat = e.element->component_type != kFloat32 || (interpolation != code.interpolation.end() && interpolation->second == kConstant);
                p.inputs.push_back({ e.element->semantic, e.element->index, static_cast<uint8_t>(e.low), static_cast<uint8_t>(e.high - e.low + 1), flat });
                for (const auto &[key, at] : layout.inputs)
                    if (code.element(key.first, key.second) == e.element)
                        p.input_map[at] = static_cast<uint8_t>(floats + key.second - first_component(e.element->mask) - e.low);
                floats += e.high - e.low + 1;
            }
            if (floats > kAlphaInputFloats)
            {
                why = "more inputs than 12 floats";
                return std::nullopt;
            }
            // Constants: each static slot's rows from its first read to its last, then the one dynamic slot's whole range.
            if (s.dynamic.size() > 1)
            {
                why = "two dynamically indexed constant buffers";
                return std::nullopt;
            }
            uint32_t rows = 0;
            for (const auto &[slot, read] : s.rows)
            {
                if (s.dynamic.count(slot))
                    continue;
                const uint32_t first = *read.begin(), count = *read.rbegin() - first + 1;
                layout.row_base[slot] = rows;
                layout.row_first[slot] = first;
                p.rows.push_back({ static_cast<uint8_t>(slot), static_cast<uint16_t>(first), static_cast<uint16_t>(count), false });
                rows += count;
            }
            if (rows > kAlphaStaticRows)
            {
                why = "more than 64 constant rows";
                return std::nullopt;
            }
            if (!s.dynamic.empty())
            {
                layout.dynamic_slot = *s.dynamic.begin();
                layout.dynamic_base = rows;
                p.rows.push_back({ static_cast<uint8_t>(layout.dynamic_slot), 0, 0, true });
            }
            layout.literals.assign(s.literals.begin(), s.literals.end());
            if (layout.literals.size() > kAlphaMaxLiterals)
            {
                why = "more than 16 literals";
                return std::nullopt;
            }
            p.literals = layout.literals;

            // The code, in the shader's order, with each emitted if's and else's jump target set at its else or endif.
            std::map<int, size_t> placed;   // an emitted if or else: its instruction
            for (size_t i = 0; i < code.at.size(); i++)
            {
                const ShexInstruction &in = *code.at[i];
                const int owner = code.owner[i];
                if (owner >= 0 && s.emitted[owner])
                {
                    // The if, or the else that follows it, jumps past its own branch to here.
                    if (in.opcode == opcode::else_)
                    {
                        placed[int(i)] = p.code.size();
                        p.code.push_back(instruction(alpha_op::else_, 0, 0, false, 0));
                        p.code[placed[owner]][0] |= static_cast<uint32_t>(p.code.size()) << 20;
                    }
                    else
                    {
                        const auto branch = std::find_if(placed.rbegin(), placed.rend(), [&](const auto &x) {
                            return x.first == owner || (code.owner[x.first] == owner && code.at[x.first]->opcode == opcode::else_);
                        });
                        p.code[branch->second][0] |= static_cast<uint32_t>(p.code.size()) << 20;
                    }
                    continue;
                }
                if (!s.emitted[i])
                    continue;
                if (in.opcode == opcode::if_)
                {
                    placed[int(i)] = p.code.size();
                    p.code.push_back(instruction(in.nonzero() ? alpha_op::if_nz : alpha_op::if_z, 0, 0, false, 0));
                    p.code.back()[1] = source(layout, in.operands[0]);
                    continue;
                }
                if (in.opcode == opcode::discard)
                {
                    p.code.push_back(instruction(in.nonzero() ? alpha_op::discard_nz : alpha_op::discard_z, 0, 0, false, 0));
                    p.code.back()[1] = source(layout, in.operands[0]);
                    continue;
                }
                const Operation operation = *operation_of(in.opcode);
                std::array<uint32_t, 4> out = instruction(operation.op, layout.temps.at(in.operands[0].index[0].value), s.mask[i], in.saturate(), 0);
                const size_t sources = operation.shape == Shape::sample ? 2 : std::min<size_t>(in.operands.size(), 4);
                for (size_t k = 1; k < sources; k++)
                    out[k] = source(layout, in.operands[k]);
                p.code.push_back(out);
            }
            if (p.code.size() > kAlphaMaxCode)
            {
                why = "more than 48 instructions";
                return std::nullopt;
            }
            return p;
        }
    }

    uint32_t AlphaProgram::attribute_floats() const
    {
        uint32_t n = 0;
        for (const AlphaInput &i : inputs)
            n += i.count;
        return n;
    }

    uint32_t AlphaProgram::static_rows() const
    {
        uint32_t n = 0;
        for (const CbRows &r : rows)
            n += r.dynamic ? 0 : r.count;
        return n;
    }

    std::vector<uint32_t> AlphaProgram::words() const
    {
        std::vector<uint32_t> out(8, 0);
        uint32_t flat = 0, at = 0;
        for (const AlphaInput &i : inputs)
        {
            if (i.flat)
                flat |= ((1u << i.count) - 1) << at;
            at += i.count;
        }
        out[0] = static_cast<uint32_t>(code.size()) | static_cast<uint32_t>(literals.size()) << 16;
        out[1] = attribute_floats() | flat << 16;
        for (uint32_t k = 0; k < kAlphaInputFloats; k++)
            out[2 + k / 4] |= uint32_t(input_map[k]) << (8 * (k % 4));
        for (const auto &v : code)
            out.insert(out.end(), v.begin(), v.end());
        for (const auto &v : literals)
            out.insert(out.end(), v.begin(), v.end());
        return out;
    }

    std::vector<SoEntry> AlphaProgram::so_entries() const
    {
        std::vector<SoEntry> out;
        for (const AlphaInput &i : inputs)
            out.push_back({ i.semantic, i.index, i.start, i.count, 0 });
        return out;
    }

    std::string AlphaProgram::describe() const
    {
        std::string out;
        for (const AlphaInput &i : inputs)
        {
            out += (out.empty() ? "" : " ") + i.semantic + std::to_string(i.index) + '.';
            for (uint32_t c = i.start; c < uint32_t(i.start) + i.count; c++)
                out += "xyzw"[c];
        }
        return out.empty() ? "-" : out;
    }

    uint32_t AlphaBuffer::add_program(const std::shared_ptr<const AlphaProgram> &program)
    {
        for (const auto &[known, offset] : program_offsets_)
            if (known == program.get())
                return offset;
        const uint32_t offset = static_cast<uint32_t>(programs_.size());
        const std::vector<uint32_t> words = program->words();
        programs_.insert(programs_.end(), words.begin(), words.end());
        program_offsets_.push_back({ program.get(), offset });
        return offset;
    }

    uint32_t AlphaBuffer::add_draw(uint32_t program, uint32_t tile, uint32_t address, const std::vector<uint32_t> &rows)
    {
        const uint32_t offset = static_cast<uint32_t>(draws_.size());
        draws_.insert(draws_.end(), { program, tile, address, static_cast<uint32_t>(rows.size() / 4) });
        draws_.insert(draws_.end(), rows.begin(), rows.begin() + static_cast<std::ptrdiff_t>(rows.size() / 4 * 4));
        return offset;
    }

    uint32_t AlphaBuffer::add_record(uint32_t draw, const float *corners, uint32_t floats)
    {
        if (records_.size() + 1 + 3 * floats >= kNoRecord)
            return kNoRecord;
        const uint32_t offset = static_cast<uint32_t>(records_.size());
        records_.push_back(draw);
        const size_t at = records_.size();
        records_.resize(at + 3 * floats);
        std::memcpy(records_.data() + at, corners, size_t(3) * floats * sizeof(float));
        return offset;
    }

    std::vector<uint32_t> AlphaBuffer::words(const std::vector<uint8_t> &atlas) const
    {
        const uint32_t programs = 4, draws = programs + static_cast<uint32_t>(programs_.size());
        const uint32_t records = draws + static_cast<uint32_t>(draws_.size()), atlas_at = records + static_cast<uint32_t>(records_.size());
        std::vector<uint32_t> out = { records, programs, draws, atlas_at };
        out.reserve(atlas_at + (atlas.size() + 3) / 4);
        out.insert(out.end(), programs_.begin(), programs_.end());
        out.insert(out.end(), draws_.begin(), draws_.end());
        out.insert(out.end(), records_.begin(), records_.end());
        out.resize(atlas_at + (atlas.size() + 3) / 4, 0);
        if (!atlas.empty())
            std::memcpy(out.data() + atlas_at, atlas.data(), atlas.size());
        return out;
    }

    std::string check_alpha_program(const AlphaProgram &p)
    {
        if (p.code.empty() || p.code.size() > kAlphaMaxCode)
            return "instructions " + std::to_string(p.code.size()) + ", not 1 to 48";
        if (p.literals.size() > kAlphaMaxLiterals)
            return "more than 16 literals";
        const uint32_t registers = kAlphaTemps + kAlphaInputFloats / 4;
        for (uint32_t pc = 0; pc < p.code.size(); pc++)
        {
            const std::array<uint32_t, 4> &in = p.code[pc];
            const uint32_t op = in[0] & 255, target = in[0] >> 20, at = pc;
            const auto where = [at](const std::string &what) { return what + " at instruction " + std::to_string(at); };
            const bool jump = op == alpha_op::if_nz || op == alpha_op::if_z || op == alpha_op::else_;
            if (op > alpha_op::utof && !jump && op != alpha_op::discard_nz && op != alpha_op::discard_z)
                return where("operation " + std::to_string(op));
            if (jump && (target <= pc || target > p.code.size()))
                return where("a jump to " + std::to_string(target) + ", not forward within the program");
            if (!jump && target != 0)
                return where("a jump target on an operation that does not jump");
            if ((in[0] >> 8 & 15) >= kAlphaTemps)
                return where("destination r" + std::to_string(in[0] >> 8 & 15));
            for (int k = 1; k < 4; k++)
            {
                const uint32_t source = in[k], kind = source & 3, index = source >> 18;
                if (kind == 0 && index >= registers)
                    return where("register " + std::to_string(index));
                if (kind == 2 && (source >> 12 & 15) >= registers)
                    return where("an offset register " + std::to_string(source >> 12 & 15));
                if (kind == 3 && index >= p.literals.size())
                    return where("literal " + std::to_string(index));
            }
        }
        return {};
    }

    AlphaAnalysis analyze_alpha(const uint8_t *bytes, size_t size)
    {
        AlphaAnalysis out;
        const std::optional<Shex> shex = decode_shex(bytes, size);
        std::optional<std::vector<SignatureElement>> inputs = parse_input_elements(bytes, size);
        if (!shex || !inputs || shex->stage != ShaderStageKind::ps)
        {
            out.why = "decode";
            return out;
        }
        const Code code = index_code(*shex, std::move(*inputs));
        std::vector<int> kept;
        for (int i = 0; i < int(code.at.size()); i++)
        {
            if (code.at[i]->opcode != opcode::discard)
                continue;
            const Slice s = slice(code, { i });
            DiscardKind kind = s.dither ? DiscardKind::dither : s.samples ? DiscardKind::texture : DiscardKind::other;
            std::string why = s.unsupported;
            const auto control = std::find_if(code.at.begin(), code.at.begin() + i, [](const ShexInstruction *in) { return unstructured(in->opcode); });
            if (kind != DiscardKind::dither && why.empty() && (control != code.at.begin() + i || !code.balanced))
                why = control != code.at.begin() + i ? opcode_name((*control)->opcode) : "unbalanced if";
            if (kind != DiscardKind::dither && !why.empty())
            {
                kind = DiscardKind::unsupported;
                if (out.why.empty())
                    out.why = why;
            }
            out.discards.push_back(kind);
            if (kind == DiscardKind::texture || kind == DiscardKind::other)
                kept.push_back(i);
        }
        if (kept.empty() || !out.why.empty())
            return out;
        // build looks up what the slice collected, so a lookup that fails is a slice this code does not understand.
        try
        {
            std::optional<AlphaProgram> program = build(code, slice(code, kept), out.why);
            const std::string invalid = program ? check_alpha_program(*program) : std::string();
            if (!invalid.empty())
                out.why = "invalid program: " + invalid;
            else if (program)
                out.program = std::make_shared<const AlphaProgram>(std::move(*program));
        }
        catch (const std::out_of_range &)
        {
            out.why = "a register the slice did not collect";
        }
        return out;
    }
}
