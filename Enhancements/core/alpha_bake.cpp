// The alpha mask bake, and shaders/alpha.hlsli's interpreter compiled as C++ through hlsl.h, which the bake runs and the
// tests take as the reference: alpha_keeps.
#include "alpha_bake.h"

#include "hlsl.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <map>
#include <thread>

namespace enhancements::shader::program
{
    namespace
    {
        // The alpha buffer the current thread's programs run on, and the interpreter's registers.
        thread_local const std::vector<uint32_t> *buffer = nullptr;
        thread_local uint4 registers[14];

        // The bake's hold on a program's sample: while active, a sample gives value, and the first one's coordinate is
        // kept, with whether a later one's differs.
        struct Probe
        {
            bool active = false;
            float value = 0;
            uint32_t samples = 0;
            float u = 0, v = 0;
            bool differs = false;
        };
        thread_local Probe probe;

        uint alpha_word(uint index) { return buffer && index < buffer->size() ? (*buffer)[index] : 0u; }
        uint4 alpha_row(uint index) { return { alpha_word(index), alpha_word(index + 1), alpha_word(index + 2), alpha_word(index + 3) }; }
        float probe_sample(uint first, uint address, float u, float v);
    }

#define LOOP
#define ALPHA_STORE(lane, r, value) ((void)(lane), registers[(r)] = (value))
#define ALPHA_LOAD(lane, r) ((void)(lane), registers[(r)])
#define ALPHA_SAMPLE(first, address, u, v) probe_sample(first, address, u, v)
#include "../shaders/alpha.hlsli"
#undef ALPHA_SAMPLE
#undef ALPHA_LOAD
#undef ALPHA_STORE
#undef LOOP

    namespace
    {
        float probe_sample(uint first, uint address, float u, float v)
        {
            if (!probe.active)
                return mask_sample(first, address, u, v);
            if (probe.samples == 0)
            {
                probe.u = u;
                probe.v = v;
            }
            else if (!(u == probe.u && v == probe.v))
                probe.differs = true;
            probe.samples++;
            return probe.value;
        }
    }

    static_assert(kAlphaRegisters == sizeof registers / sizeof registers[0] && kAlphaInputs == kAlphaTemps && kOpaqueRecord == kNoRecord
        && kAlphaCodeCap == kAlphaMaxCode && kMaskHeaderWords == enhancements::kMaskHeaderWords && kMaskRecordWords == enhancements::kMaskRecordWords
        && kMaskSide == kAtlasTile);
    static_assert(kOpMov == alpha_op::mov && kOpMovc == alpha_op::movc && kOpAdd == alpha_op::add && kOpMul == alpha_op::mul && kOpMad == alpha_op::mad
        && kOpDiv == alpha_op::div && kOpMin == alpha_op::min && kOpMax == alpha_op::max && kOpDp2 == alpha_op::dp2 && kOpDp3 == alpha_op::dp3
        && kOpDp4 == alpha_op::dp4 && kOpLt == alpha_op::lt && kOpGe == alpha_op::ge && kOpEq == alpha_op::eq && kOpNe == alpha_op::ne
        && kOpFtoi == alpha_op::ftoi && kOpFtou == alpha_op::ftou && kOpRoundNe == alpha_op::round_ne && kOpRoundNi == alpha_op::round_ni
        && kOpRoundPi == alpha_op::round_pi && kOpRoundZ == alpha_op::round_z && kOpFrc == alpha_op::frc && kOpRsq == alpha_op::rsq
        && kOpSqrt == alpha_op::sqrt && kOpExp == alpha_op::exp && kOpLog == alpha_op::log && kOpRcp == alpha_op::rcp && kOpSample == alpha_op::sample
        && kOpAnd == alpha_op::and_ && kOpOr == alpha_op::or_ && kOpXor == alpha_op::xor_ && kOpNot == alpha_op::not_ && kOpIadd == alpha_op::iadd
        && kOpIeq == alpha_op::ieq && kOpIne == alpha_op::ine && kOpIlt == alpha_op::ilt && kOpIge == alpha_op::ige && kOpUlt == alpha_op::ult
        && kOpUge == alpha_op::uge && kOpImin == alpha_op::imin && kOpImax == alpha_op::imax && kOpUmin == alpha_op::umin && kOpUmax == alpha_op::umax
        && kOpIneg == alpha_op::ineg && kOpIshl == alpha_op::ishl && kOpIshr == alpha_op::ishr && kOpUshr == alpha_op::ushr && kOpItof == alpha_op::itof
        && kOpUtof == alpha_op::utof && kOpIfNz == alpha_op::if_nz && kOpIfZ == alpha_op::if_z && kOpElse == alpha_op::else_
        && kOpDiscardNz == alpha_op::discard_nz && kOpDiscardZ == alpha_op::discard_z);
}

namespace enhancements
{
    const std::array<const char *, static_cast<size_t>(BakeClass::count)> kBakeClassNames = { "exact", "approximate", "fallback" };

    namespace
    {
        namespace program = shader::program;

        constexpr uint32_t kSampleBit = 1u << 31;
        constexpr float kThird = 1.0f / 3.0f;
        // A coordinate at the centroid this far, in the texture's units, from its corners' interpolation is not affine.
        constexpr float kAffine = 1.0e-4f;
        // One sweep of the verdict over the sample's byte values for this many cutoffs a draw computes.
        constexpr uint32_t kSweepEvery = 64;
        // Halvings of an approximate cutoff: within 1/4096, a sixteenth of the mask's byte step, where the inputs it holds
        // at the centroid move the program's own cutoff over the triangle by more.
        constexpr uint32_t kApproximateSteps = 12;

        // Sources an operation reads, as core/alpha.cpp encodes them; the others are 0.
        uint32_t arity(uint32_t op)
        {
            using namespace alpha_op;
            if (op == movc || op == mad)
                return 3;
            if (op == mov || op == ftoi || op == ftou || op == round_ne || op == round_ni || op == round_pi || op == round_z || op == frc || op == rsq
                || op == sqrt || op == exp || op == log || op == rcp || op == not_ || op == ineg || op == itof || op == utof || op == sample)
                return 1;
            return 2;
        }

        AlphaFlow flow_of(const uint32_t *code, uint32_t count, const uint8_t *map)
        {
            // Each register component's inputs: attribute float bits, and kSampleBit for the sample's value.
            uint32_t reg[11][4] = {};
            for (uint32_t r = 0; r < 3; r++)
                for (uint32_t c = 0; c < 4; c++)
                    reg[8 + r][c] = map[r * 4 + c] < 31 ? 1u << map[r * 4 + c] : 0u;
            const auto source = [&](uint32_t s, uint32_t k) -> uint32_t {
                const uint32_t kind = s & 3, index = s >> 18;
                if (kind == 0)
                    return index < 11 ? reg[index][s >> (2 + 2 * k) & 3] : 0u;
                if (kind == 2)
                {
                    const uint32_t r = s >> 12 & 15;
                    return r < 11 ? reg[r][s >> 16 & 3] : 0u;
                }
                return 0u;
            };
            // The ifs around the current instruction: where each ends and what its condition reads.
            std::vector<std::pair<uint32_t, uint32_t>> ifs;
            AlphaFlow f;
            for (uint32_t pc = 0; pc < count; pc++)
            {
                const uint32_t *in = code + size_t(pc) * 4;
                const uint32_t op = in[0] & 255, target = in[0] >> 20;
                ifs.erase(std::remove_if(ifs.begin(), ifs.end(), [pc](const auto &i) { return i.first <= pc; }), ifs.end());
                uint32_t control = 0;
                for (const auto &i : ifs)
                    control |= i.second;
                if (op == alpha_op::if_nz || op == alpha_op::if_z)
                {
                    // Its branch, and the else's after it when the if jumps past an else.
                    uint32_t end = target;
                    if (target > pc + 1 && target <= count && (code[size_t(target - 1) * 4] & 255) == alpha_op::else_)
                        end = code[size_t(target - 1) * 4] >> 20;
                    ifs.push_back({ end, source(in[1], 0) | control });
                    continue;
                }
                if (op == alpha_op::else_)
                    continue;
                if (op == alpha_op::discard_nz || op == alpha_op::discard_z)
                {
                    f.decision |= source(in[1], 0) | control;
                    continue;
                }
                uint32_t t[4] = {};
                if (op == alpha_op::sample)
                {
                    f.samples++;
                    f.coordinate |= source(in[1], 0) | source(in[1], 1);
                    std::fill(t, t + 4, kSampleBit);
                }
                else if (op == alpha_op::dp2 || op == alpha_op::dp3 || op == alpha_op::dp4)
                {
                    uint32_t all = 0;
                    for (uint32_t k = 0; k < op - alpha_op::dp2 + 2; k++)
                        all |= source(in[1], k) | source(in[2], k);
                    std::fill(t, t + 4, all);
                }
                else
                    for (uint32_t k = 0; k < 4; k++)
                        for (uint32_t j = 0; j < arity(op); j++)
                            t[k] |= source(in[1 + j], k);
                const uint32_t dst = std::min<uint32_t>(in[0] >> 8 & 15, kAlphaTemps - 1), mask = in[0] >> 12 & 15;
                for (uint32_t k = 0; k < 4; k++)
                    if (mask >> k & 1)
                        reg[dst][k] = control != 0 ? reg[dst][k] | t[k] | control : t[k];
            }
            f.reads_sample = (f.decision & kSampleBit) != 0;
            f.decision &= ~kSampleBit;
            return f;
        }

        // A triangle's verdict: kept everywhere, nowhere, where the sample reaches the cutoff (raise) or stays at most at
        // it (lower), or not baked.
        enum class Verdict : uint8_t { always, never, raise, lower, fallback };

        struct Baked
        {
            BakeClass cls = BakeClass::fallback;
            Verdict verdict = Verdict::fallback;
            float cutoff = 0;
            float uv[6] = {};
            const char *why = nullptr;
        };

        // A draw's alpha-tested triangles: indices into the triangles, and their records.
        struct Group
        {
            uint32_t block = 0;
            std::vector<uint32_t> triangles, records;
        };

        float as_float(uint32_t u)
        {
            float f;
            std::memcpy(&f, &u, 4);
            return f;
        }

        uint32_t as_bits(float f)
        {
            uint32_t u;
            std::memcpy(&u, &f, 4);
            return u;
        }

        // The program's verdict at barycentric (u, v) of record when its sample gives s.
        bool keeps_with(uint32_t record, float u, float v, float s)
        {
            program::probe = {};
            program::probe.active = true;
            program::probe.value = s;
            const bool keeps = program::alpha_test(record, u, v, 0, 0).keeps != 0;
            program::probe.active = false;
            return keeps;
        }

        // The sample's coordinate at barycentric (u, v), the program run to its end whatever its discards do; samples 0
        // when it takes none there.
        program::Probe coordinate_at(uint32_t record, float u, float v)
        {
            program::probe = {};
            program::probe.active = true;
            program::probe.value = 0.5f;
            program::alpha_test(record, u, v, 0, 1);
            const program::Probe out = program::probe;
            program::probe.active = false;
            return out;
        }

        // How often the verdict changes over the sample's 256 byte values.
        uint32_t changes(uint32_t record, float u, float v)
        {
            uint32_t n = 0;
            bool last = keeps_with(record, u, v, 0.0f);
            for (uint32_t i = 1; i < 256; i++)
            {
                const bool k = keeps_with(record, u, v, float(i) / 255.0f);
                n += k != last;
                last = k;
            }
            return n;
        }

        // The cutoff where the verdict at (u, v) changes, between a sample of 0 and of 1 which give different verdicts:
        // for a raise the least float that keeps, for a lower the largest. Exact: bisects the floats' bits, which order the
        // non-negative floats, so for a test that changes once the cutoff gives the program's verdict for every sample.
        // Otherwise, for a verdict held at the centroid, to kApproximateSteps halvings of the samples' range.
        float cutoff_of(uint32_t record, float u, float v, bool raise, bool exact)
        {
            if (!exact)
            {
                float lo = 0.0f, hi = 1.0f;
                for (uint32_t k = 0; k < kApproximateSteps; k++)
                {
                    const float mid = (lo + hi) * 0.5f;
                    (keeps_with(record, u, v, mid) == raise ? hi : lo) = mid;
                }
                return raise ? hi : lo;
            }
            uint32_t lo = as_bits(0.0f), hi = as_bits(1.0f);
            while (hi - lo > 1)
            {
                const uint32_t mid = lo + (hi - lo) / 2;
                (keeps_with(record, u, v, as_float(mid)) == raise ? hi : lo) = mid;
            }
            return as_float(raise ? hi : lo);
        }


        // Bakes one draw's triangles into out, at their places in the alpha triangles.
        void bake_group(const std::vector<uint32_t> &a, const Group &g, std::vector<Baked> &out, std::vector<BakeDraw> &draws, size_t index)
        {
            const uint32_t records_at = a[0], programs_at = a[1], draws_at = a[2], atlas_at = a[3];
            const uint32_t block = draws_at + g.block;
            const uint32_t program = block < a.size() ? a[block] : 0;
            const uint32_t tile = block + 1 < a.size() ? a[block + 1] : kNoTile;
            const uint32_t p = programs_at + program;
            BakeDraw &draw = draws[index];
            draw.program = program;
            // The whole draw falls back, whatever its triangles found so far.
            const auto fail = [&](const char *why) {
                for (uint32_t t : g.triangles)
                {
                    out[t] = Baked {};
                    out[t].why = why;
                }
                draw.triangles = {};
                draw.triangles[static_cast<size_t>(BakeClass::fallback)] = static_cast<uint32_t>(g.triangles.size());
                draw.why = why;
            };
            if (p + 8 > a.size())
                return fail("a program past the buffer");
            const uint32_t count = a[p] & 0xFFFF, floats = a[p + 1] & 0xFFFF, flat = a[p + 1] >> 16;
            if (count == 0 || count > kAlphaMaxCode || p + 8 + size_t(count) * 4 > a.size() || floats > kAlphaInputFloats)
                return fail("a program the interpreter cannot run");
            uint8_t map[kAlphaInputFloats];
            for (uint32_t k = 0; k < kAlphaInputFloats; k++)
                map[k] = static_cast<uint8_t>(a[p + 2 + k / 4] >> (8 * (k % 4)));
            const AlphaFlow flow = flow_of(a.data() + p + 8, count, map);
            if (flow.reads_sample && (tile == kNoTile || uint64_t(atlas_at) + (uint64_t(tile) + 1) * (kMaskSlotBytes / 4) > a.size()))
                return fail("no texture tile");
            if ((flow.coordinate & kSampleBit) != 0)
                return fail("a coordinate that reads a sample");

            // Cutoffs by the values of the inputs the verdict reads besides the sample.
            std::map<std::vector<uint32_t>, std::pair<Verdict, float>> cache;
            uint32_t computed = 0;
            for (size_t i = 0; i < g.triangles.size(); i++)
            {
                Baked &b = out[g.triangles[i]];
                const uint32_t r = g.records[i];
                const uint32_t at = records_at + r + 1;
                if (at + 3 * floats > a.size())
                {
                    b.why = "a record past the buffer";
                    draw.triangles[static_cast<size_t>(BakeClass::fallback)]++;
                    continue;
                }
                // The inputs the verdict reads, at the centroid as the interpreter interpolates them, and whether each is
                // the same over the whole triangle.
                std::vector<uint32_t> key;
                bool constant = true;
                for (uint32_t f = 0; f < floats; f++)
                {
                    if ((flow.decision >> f & 1) == 0)
                        continue;
                    const uint32_t c0 = a[at + f], c1 = a[at + floats + f], c2 = a[at + 2 * floats + f];
                    const bool is_flat = (flat >> f & 1) != 0;
                    constant &= is_flat || (c0 == c1 && c1 == c2);
                    const float w = 1.0f - kThird - kThird;
                    key.push_back(is_flat ? c0 : as_bits(as_float(c0) * w + as_float(c1) * kThird + as_float(c2) * kThird));
                }
                auto known = cache.find(key);
                if (known == cache.end())
                {
                    std::pair<Verdict, float> v { Verdict::always, 0.0f };
                    const bool k0 = keeps_with(r, kThird, kThird, 0.0f);
                    const bool k1 = flow.reads_sample ? keeps_with(r, kThird, kThird, 1.0f) : k0;
                    if (flow.reads_sample && computed++ % kSweepEvery == 0 && changes(r, kThird, kThird) > 1)
                        return fail("a verdict that changes more than once with the sample");
                    if (k0 == k1)
                        v.first = k0 ? Verdict::always : Verdict::never;
                    else
                        v = { k1 ? Verdict::raise : Verdict::lower, cutoff_of(r, kThird, kThird, k1, constant) };
                    known = cache.emplace(std::move(key), v).first;
                }
                b.verdict = known->second.first;
                b.cutoff = known->second.second;
                b.cls = constant ? BakeClass::exact : BakeClass::approximate;
                if (b.verdict == Verdict::raise || b.verdict == Verdict::lower)
                {
                    const float corners[3][2] = { { 0, 0 }, { 1, 0 }, { 0, 1 } };
                    for (int c = 0; c < 3 && b.verdict != Verdict::fallback; c++)
                    {
                        const program::Probe probe = coordinate_at(r, corners[c][0], corners[c][1]);
                        if (probe.samples == 0 || probe.differs)
                        {
                            b.verdict = Verdict::fallback;
                            b.why = probe.samples == 0 ? "a corner that takes no sample" : "two coordinates";
                        }
                        b.uv[c * 2] = probe.u;
                        b.uv[c * 2 + 1] = probe.v;
                    }
                    if (b.verdict != Verdict::fallback)
                    {
                        // The coordinate interpolates as the corners' only when the program computes it affinely.
                        const program::Probe centroid = coordinate_at(r, kThird, kThird);
                        const float w = 1.0f - kThird - kThird;
                        const float u = b.uv[0] * w + b.uv[2] * kThird + b.uv[4] * kThird, v = b.uv[1] * w + b.uv[3] * kThird + b.uv[5] * kThird;
                        if (!(std::fabs(centroid.u - u) <= kAffine * (1 + std::fabs(u)) && std::fabs(centroid.v - v) <= kAffine * (1 + std::fabs(v))))
                            b.cls = BakeClass::approximate;
                    }
                }
                if (b.verdict == Verdict::fallback)
                {
                    b.cls = BakeClass::fallback;
                    if (draw.why.empty())
                        draw.why = b.why;
                }
                draw.triangles[static_cast<size_t>(b.cls)]++;
            }
        }
    }

    AlphaFlow alpha_flow(const std::vector<std::array<uint32_t, 4>> &code, const std::array<uint8_t, kAlphaInputFloats> &input_map)
    {
        return flow_of(code.empty() ? nullptr : code[0].data(), static_cast<uint32_t>(code.size()), input_map.data());
    }

    BakeClass bake_class(const AlphaProgram &program, std::string &why)
    {
        const AlphaFlow flow = alpha_flow(program.code, program.input_map);
        uint32_t at = 0;
        why.clear();
        for (const AlphaInput &input : program.inputs)
            for (uint32_t c = 0; c < input.count; c++, at++)
                if ((flow.decision >> at & 1) != 0 && !input.flat)
                    why += (why.empty() ? "" : " ") + input.semantic + std::to_string(input.index) + '.' + "xyzw"[input.start + c];
        return why.empty() ? BakeClass::exact : BakeClass::approximate;
    }

    bool alpha_keeps(const std::vector<uint32_t> &alpha, uint32_t record, float u, float v)
    {
        program::buffer = &alpha;
        const bool keeps = program::alpha_test(record, u, v, 0, 0).keeps != 0;
        program::buffer = nullptr;
        return keeps;
    }

    AlphaMasks bake_alpha(const std::vector<uint32_t> &a, std::vector<TraceTriangle> &triangles, uint32_t tasks)
    {
        AlphaMasks out;
        AlphaBakeCounts &counts = out.counts;
        // The alpha triangles in order, grouped by draw.
        std::vector<uint32_t> alpha_triangles, alpha_records;
        std::vector<Group> groups;
        std::vector<uint32_t> group_of;
        std::map<uint32_t, uint32_t> by_block;
        for (uint32_t i = 0; i < triangles.size(); i++)
        {
            const uint32_t record = triangles[i].info & kOpaque;
            if (record == kOpaque)
                continue;
            if (a.size() < 4 || uint64_t(a[0]) + record >= a.size())
            {
                triangles[i].info = triangle_info(info_class(triangles[i].info));
                counts.triangles[static_cast<size_t>(BakeClass::fallback)]++;
                if (counts.why.empty())
                    counts.why = "a record past the buffer";
                continue;
            }
            const uint32_t block = a[a[0] + record];
            const auto [it, added] = by_block.emplace(block, static_cast<uint32_t>(groups.size()));
            if (added)
                groups.push_back({ block, {}, {} });
            groups[it->second].triangles.push_back(static_cast<uint32_t>(alpha_triangles.size()));
            groups[it->second].records.push_back(record);
            group_of.push_back(it->second);
            alpha_triangles.push_back(i);
            alpha_records.push_back(record);
        }
        std::vector<Baked> baked(alpha_triangles.size());
        out.draws.resize(groups.size());
        std::atomic<size_t> next { 0 };
        const auto work = [&] {
            program::buffer = &a;
            for (size_t g; (g = next++) < groups.size();)
                bake_group(a, groups[g], baked, out.draws, g);
            program::buffer = nullptr;
        };
        std::vector<std::thread> threads;
        for (uint32_t t = 1; t < std::max(1u, tasks); t++)
            threads.emplace_back(work);
        work();
        for (std::thread &t : threads)
            t.join();

        // The records and masks in the triangles' order: a mask by tile and complement, in order of first use.
        std::vector<uint32_t> records;
        std::map<std::pair<uint32_t, bool>, uint32_t> masks;
        std::vector<std::pair<uint32_t, bool>> mask_order;
        for (size_t i = 0; i < alpha_triangles.size(); i++)
        {
            const Baked &b = baked[i];
            TraceTriangle &t = triangles[alpha_triangles[i]];
            const TraceClass c = info_class(t.info);
            counts.triangles[static_cast<size_t>(b.cls)]++;
            if (b.cls == BakeClass::fallback && counts.why.empty() && b.why)
                counts.why = b.why;
            counts.always += b.verdict == Verdict::always;
            counts.never += b.verdict == Verdict::never;
            if (b.verdict == Verdict::fallback || b.verdict == Verdict::always)
            {
                t.info = triangle_info(c);
                continue;
            }
            if (out.sources.size() >= kMaxMaskRecords)
            {
                t.info = triangle_info(c);
                counts.past_max++;
                continue;
            }
            const uint32_t block = a[2] + groups[group_of[i]].block;
            const uint32_t tile = a[block + 1], address = a[block + 2] & 63;
            uint32_t word0 = 0;
            float cutoff = 2.0f;    // never: no sample reaches it
            if (b.verdict != Verdict::never)
            {
                const auto key = std::pair { tile, b.verdict == Verdict::lower };
                auto it = masks.find(key);
                if (it == masks.end())
                {
                    it = masks.emplace(key, static_cast<uint32_t>(mask_order.size())).first;
                    mask_order.push_back(key);
                }
                word0 = it->second | address << 12;   // the slot is offset once the records' size is known
                cutoff = b.verdict == Verdict::raise ? b.cutoff : 1.0f - b.cutoff;
            }
            t.info = triangle_info(c, static_cast<uint32_t>(out.sources.size()));
            out.sources.push_back(alpha_records[i]);
            out.classes.push_back(b.cls);
            records.push_back(b.verdict == Verdict::never ? 0xFFFFFFFFu : word0);
            records.push_back(as_bits(cutoff));
            for (float f : b.uv)
                records.push_back(as_bits(f));
        }
        counts.records = static_cast<uint32_t>(out.sources.size());
        counts.masks = static_cast<uint32_t>(mask_order.size());
        for (const BakeDraw &d : out.draws)
        {
            size_t worst = 0;
            for (size_t k = 0; k < d.triangles.size(); k++)
                worst = d.triangles[k] != 0 ? k : worst;
            counts.draws[worst]++;
        }
        std::map<uint32_t, size_t> program_worst;
        for (const BakeDraw &d : out.draws)
        {
            size_t worst = 0;
            for (size_t k = 0; k < d.triangles.size(); k++)
                worst = d.triangles[k] != 0 ? k : worst;
            program_worst[d.program] = std::max(program_worst[d.program], worst);
        }
        for (const auto &[program, worst] : program_worst)
            counts.programs[worst]++;
        if (records.empty())
            return out;

        const uint32_t record_words = kMaskHeaderWords + static_cast<uint32_t>(records.size());
        const uint32_t first_slot = (record_words * 4 + kMaskSlotBytes - 1) / kMaskSlotBytes;
        out.words.assign(size_t(first_slot + mask_order.size()) * (kMaskSlotBytes / 4), 0);
        out.words[0] = kMaskHeaderWords;
        out.words[1] = counts.records;
        out.words[2] = first_slot;
        out.words[3] = counts.masks;
        for (size_t r = 0; r < records.size(); r += kMaskRecordWords)
        {
            // A never record reads the buffer's first slot, whose bytes no sample can make reach its cutoff.
            records[r] = records[r] == 0xFFFFFFFFu ? 0u : records[r] + first_slot;
        }
        std::copy(records.begin(), records.end(), out.words.begin() + kMaskHeaderWords);
        for (size_t m = 0; m < mask_order.size(); m++)
        {
            const auto [tile, lower] = mask_order[m];
            const uint8_t *from = reinterpret_cast<const uint8_t *>(a.data() + a[3]) + size_t(tile) * kMaskSlotBytes;
            uint8_t *to = reinterpret_cast<uint8_t *>(out.words.data() + (first_slot + m) * (kMaskSlotBytes / 4));
            for (uint32_t k = 0; k < kMaskSlotBytes; k++)
                to[k] = lower ? static_cast<uint8_t>(255 - from[k]) : from[k];
        }
        return out;
    }
}
