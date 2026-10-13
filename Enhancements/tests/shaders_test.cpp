// The shader registry: the census's hash of a real shader, and bytecode shared by hash and freed with its last handle.
#include "../core/shaders.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iterator>
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
}

int main()
{
    std::ifstream file("fixtures/d13184314dc3a9c2.vs.dxbc", std::ios::binary);
    const std::vector<char> vs((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    check("the hash of fixtures/d13184314dc3a9c2.vs.dxbc is the census's d13184314dc3a9c2", shader_hash(vs.data(), vs.size()) == 0xd13184314dc3a9c2ull);
    check("the hash of no bytes is FNV-1a's offset basis", shader_hash(nullptr, 0) == 0xcbf29ce484222325ull);

    ShaderRegistry registry;
    const std::string ds = "domain shader bytes";
    registry.add(0x1000, ShaderStage::vertex, vs.data(), vs.size());
    registry.add(0x2000, ShaderStage::vertex, vs.data(), vs.size());
    registry.add(0x3000, ShaderStage::domain, ds.data(), ds.size());
    const auto a = registry.find(0x1000), b = registry.find(0x2000), c = registry.find(0x3000);
    check("two handles of the same bytecode find it with its stage and hash", a && b && a->stage == ShaderStage::vertex && a->hash == 0xd13184314dc3a9c2ull
        && b->hash == a->hash && a->code->size() == vs.size() && std::equal(vs.begin(), vs.end(), a->code->begin()));
    check("the two handles share one copy, held once", a && b && a->code == b->code && registry.bytes() == vs.size() + ds.size());
    check("the domain shader keeps its stage and bytes", c && c->stage == ShaderStage::domain && *c->code == ds);
    check("an unknown handle finds nothing", !registry.find(0x4000));
    registry.remove(0x1000);
    check("removing one handle keeps the other's bytecode", !registry.find(0x1000) && registry.find(0x2000) && registry.bytes() == vs.size() + ds.size());
    registry.remove(0x2000);
    check("removing the last handle frees the bytecode", !registry.find(0x2000) && registry.bytes() == ds.size());
    check("a copy found earlier stays valid after its handles are gone", a && a->code->size() == vs.size());
    registry.add(0x3000, ShaderStage::vertex, vs.data(), vs.size());
    const auto replaced = registry.find(0x3000);
    check("a handle added again replaces its entry and frees what only it held", replaced && replaced->stage == ShaderStage::vertex
        && replaced->hash == 0xd13184314dc3a9c2ull && registry.bytes() == vs.size());
    const std::string ps = "pixel shader bytes";
    registry.add(0x6000, ShaderStage::pixel, ps.data(), ps.size());
    const auto pixel = registry.find(0x6000);
    check("a pixel shader keeps its hash and its bytecode, for its alpha program", pixel && pixel->stage == ShaderStage::pixel
        && pixel->hash == shader_hash(ps.data(), ps.size()) && pixel->code && *pixel->code == ps && registry.bytes() == vs.size() + ps.size());
    registry.remove(0x6000);
    check("and is removed like any other", !registry.find(0x6000) && registry.bytes() == vs.size());
    registry.remove(0x5000);
    check("removing an unknown handle changes nothing", registry.bytes() == vs.size());
    std::printf("%s: %d failed\n", __FILE__, failures);
    return failures == 0 ? 0 : 1;
}
