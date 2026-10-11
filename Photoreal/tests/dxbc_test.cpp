// Output signatures of the game's own shaders from capture 111824, as tracer/dxbc_sig.py reads them, the stream-output
// declaration each gets, and broken containers that must give nothing.
#include "../core/dxbc.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

using namespace photoreal;

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

    std::optional<OutputSignature> parse(const std::vector<uint8_t> &code) { return parse_output_signature(code.data(), code.size()); }

    // "SV_POSITION0 TEXCOORD0 ..." in signature order.
    std::string names(const OutputSignature &s)
    {
        std::string out;
        for (const SignatureElement &e : s.elements)
            out += (out.empty() ? "" : " ") + e.semantic + std::to_string(e.index);
        return out;
    }

    bool same(const SoDeclaration &d, const char *semantic, uint32_t stride)
    {
        return d.entries.size() == 1 && d.entries[0].semantic == semantic && d.entries[0].index == 0 && d.entries[0].start == 0
            && d.entries[0].count == 4 && d.entries[0].slot == 0 && d.stride == stride;
    }

    void put32(std::vector<uint8_t> &code, size_t at, uint32_t value) { std::memcpy(code.data() + at, &value, 4); }
}

int main()
{
    const std::vector<uint8_t> packed = load("d13184314dc3a9c2.vs.dxbc"), hull_feed = load("f41ee402a971d845.vs.dxbc"),
        position_only = load("ddf425b5aabfc702.vs.dxbc"), domain = load("2adc971cc2058b7a.ds.dxbc"), pixel = load("482d98462fb30496.ps.dxbc");
    {
        const auto s = parse(packed);
        check("d13184314dc3a9c2: a vertex shader with 8 outputs", s && s->stage == ShaderStageKind::vs && s->elements.size() == 8);
        check("d13184314dc3a9c2: SV_POSITION0 is system value 1 in o0.xyzw",
            s && s->elements[0].semantic == "SV_POSITION" && s->elements[0].system_value == 1 && s->elements[0].reg == 0 && s->elements[0].mask == 0xF);
        check("d13184314dc3a9c2: TEXCOORD0.xyz and TEXCOORD4.w share o1", s && s->elements[1].semantic == "TEXCOORD" && s->elements[1].index == 0
            && s->elements[1].reg == 1 && s->elements[1].mask == 0x7 && s->elements[2].index == 4 && s->elements[2].reg == 1 && s->elements[2].mask == 0x8);
        check("d13184314dc3a9c2: the outputs in order", s && names(*s) == "SV_POSITION0 TEXCOORD0 TEXCOORD4 COLOR0 TEXCOORD1 TEXCOORD2 TEXCOORD3 SV_InstanceID0");
        check("d13184314dc3a9c2: SV_InstanceID0 in o6.x with no system value", s && s->elements[7].reg == 6 && s->elements[7].mask == 0x1
            && s->elements[7].system_value == 0);
    }
    {
        const auto s = parse(hull_feed);
        check("f41ee402a971d845, which feeds the hull shader: TEXCOORD0 TEXCOORD1 TEXCOORD2 SV_InstanceID0 and no position",
            s && s->stage == ShaderStageKind::vs && names(*s) == "TEXCOORD0 TEXCOORD1 TEXCOORD2 SV_InstanceID0");
        const auto p = parse(position_only);
        check("ddf425b5aabfc702: SV_POSITION0 in o0.xyzw alone", p && p->elements.size() == 1 && p->elements[0].system_value == 1
            && p->elements[0].reg == 0 && p->elements[0].mask == 0xF);
        const auto d = parse(domain);
        check("2adc971cc2058b7a: a domain shader whose 6 outputs start with SV_POSITION0 in o0", d && d->stage == ShaderStageKind::ds
            && names(*d) == "SV_POSITION0 TEXCOORD0 TEXCOORD1 TEXCOORD2 TEXCOORD3 SV_InstanceID0" && d->elements[0].system_value == 1);
        const auto ps = parse(pixel);
        check("482d98462fb30496: a pixel shader with SV_Target0", ps && ps->stage == ShaderStageKind::ps && names(*ps) == "SV_Target0");
    }
    {
        const auto vs = parse(packed), ds = parse(domain), feed = parse(hull_feed), only = parse(position_only);
        const auto a = so_declaration(*vs), b = so_declaration(*ds), c = so_declaration(*only);
        check("the stream output of the vertex shaders and the domain shader: SV_POSITION 0, components 0 to 3 in slot 0, 16 bytes",
            a && same(*a, "SV_POSITION", 16) && b && same(*b, "SV_POSITION", 16) && c && same(*c, "SV_POSITION", 16));
        check("no stream output for the vertex shader that feeds the hull shader", !so_declaration(*feed));
        OutputSignature moved = *vs;
        std::swap(moved.elements[0], moved.elements[3]);
        moved.elements[3].reg = 2;
        const auto by_name = so_declaration(moved);
        check("the position is found by its system value wherever its register is", by_name && same(*by_name, "SV_POSITION", 16));
    }
    {
        bool all = true;
        for (const std::vector<uint8_t> *code : { &packed, &hull_feed, &position_only, &domain, &pixel })
            for (size_t size = 0; size < code->size(); size += 16)
            {
                const std::vector<uint8_t> cut(code->begin(), code->begin() + size);
                all &= !parse_output_signature(cut.data(), cut.size());
            }
        check("each fixture cut at every 16-byte boundary gives nothing", all);
        // With the header's total size rewritten to the cut, the parser walks the chunk table into the cut chunk.
        all = true;
        for (const std::vector<uint8_t> *code : { &packed, &hull_feed, &position_only, &domain, &pixel })
            for (size_t size = 32; size < code->size(); size += 16)
            {
                std::vector<uint8_t> cut(code->begin(), code->begin() + size);
                put32(cut, 24, static_cast<uint32_t>(size));
                all &= !parse_output_signature(cut.data(), cut.size());
            }
        check("each fixture cut at every 16-byte boundary with its total size rewritten to the cut gives nothing", all);
        std::vector<uint8_t> broken = packed;
        broken[0] = 'X';
        check("a bad magic gives nothing", !parse(broken));
        broken = packed;
        put32(broken, 32 + 4, static_cast<uint32_t>(packed.size()) - 4);
        check("a chunk offset 4 bytes before the end gives nothing", !parse(broken));
        broken = packed;
        put32(broken, 32 + 4, 0x7FFFFFF0);
        check("a chunk offset past the end gives nothing", !parse(broken));
        broken = packed;
        put32(broken, 284 + 8, 1000);
        check("an element count past the signature chunk gives nothing", !parse(broken));
        broken = packed;
        put32(broken, 284 + 8 + 8, 5000);
        check("an element name past the signature chunk gives nothing", !parse(broken));
        broken = packed;
        put32(broken, 284 + 4, 9);
        check("a signature chunk 9 bytes long gives nothing", !parse(broken));
        check("a null pointer gives nothing", !parse_output_signature(nullptr, 100));
    }
    {
        // The scene capture streams a tessellated draw's triangles through a geometry shader of its own, which takes
        // triangles: a domain of tri or quad.
        const auto domain_of = [](const std::vector<uint8_t> &code) { return tessellator_domain(code.data(), code.size()); };
        check("2adc971cc2058b7a, the terrain's domain shader, declares the tri domain", domain_of(domain) == TessellatorDomain::tri);
        check("the vertex and pixel shaders declare no tessellator domain",
            !domain_of(packed) && !domain_of(hull_feed) && !domain_of(position_only) && !domain_of(pixel));
        bool none = true;
        for (size_t size = 32; size < domain.size(); size += 4)
        {
            std::vector<uint8_t> cut(domain.begin(), domain.begin() + size);
            put32(cut, 24, static_cast<uint32_t>(size));
            const auto d = tessellator_domain(cut.data(), cut.size());
            none &= !d || *d == TessellatorDomain::tri;
        }
        check("the domain shader cut at every 4-byte boundary gives tri or nothing, never another domain", none);
        check("a null pointer declares no domain", !tessellator_domain(nullptr, 100));
    }
    std::printf("%s: %d failed\n", __FILE__, failures);
    return failures == 0 ? 0 : 1;
}
