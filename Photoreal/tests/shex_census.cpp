// Census tool for core/shex.* and core/alpha.*: decodes FrameCensus shader files (<capture>/shaders/<hash>.<stage>.dxbc).
//
//   shex_census names <dxbc>...   one line per shader: its path, then each instruction's name, declarations left out
//   shex_census alpha <dxbc>...   one line per pixel shader with a discard: its kinds, and its program or why it has none,
//                                 with how the mask bake holds it: exact, or approximate with the varying inputs it reads
//
// check_shex.sh runs both over the census captures; it is not part of run.sh, since it needs .work.
#include "../core/alpha_bake.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>

using namespace photoreal;

namespace
{
    std::string bytes_of(const char *path)
    {
        std::ifstream in(path, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
}

int main(int argc, char **argv)
{
    if (argc < 2 || (std::strcmp(argv[1], "names") != 0 && std::strcmp(argv[1], "alpha") != 0))
    {
        std::fprintf(stderr, "usage: shex_census names|alpha <dxbc>...\n");
        return 2;
    }
    const bool names = std::strcmp(argv[1], "names") == 0;
    for (int i = 2; i < argc; i++)
    {
        const std::string b = bytes_of(argv[i]);
        const auto *code = reinterpret_cast<const uint8_t *>(b.data());
        if (names)
        {
            const std::optional<Shex> shex = decode_shex(code, b.size());
            std::printf("%s", argv[i]);
            if (!shex)
                std::printf(" FAILED");
            else
                for (const ShexInstruction &in : shex->instructions)
                {
                    const std::string name = instruction_name(in);
                    if (name.rfind("dcl_", 0) != 0)
                        std::printf(" %s", name.c_str());
                }
            std::printf("\n");
            continue;
        }
        if (std::strstr(argv[i], ".ps.dxbc") == nullptr)
            continue;
        const AlphaAnalysis a = analyze_alpha(code, b.size());
        if (a.discards.empty() && a.why.empty())
            continue;
        std::string kinds;
        for (DiscardKind k : a.discards)
            kinds += (kinds.empty() ? "" : ",") + std::string(kDiscardKindNames[static_cast<size_t>(k)]);
        std::printf("%s\t%s\t", argv[i], kinds.c_str());
        if (!a.program)
        {
            std::printf("%s\n", a.why.empty() ? "opaque" : ("unsupported: " + a.why).c_str());
            continue;
        }
        const AlphaProgram &p = *a.program;
        std::string rows;
        for (const CbRows &r : p.rows)
            rows += (rows.empty() ? "" : " ") + ("cb" + std::to_string(r.slot)) + (r.dynamic ? "[dynamic]" : "[" + std::to_string(r.first) + "+" + std::to_string(r.count) + "]");
        std::string varying;
        const BakeClass bake = bake_class(p, varying);
        std::printf("program %zu instructions, inputs %s, rows %s, %s, bake %s%s\n", p.code.size(), p.describe().c_str(), rows.empty() ? "-" : rows.c_str(),
            p.texture == kNoSlot ? "no texture" : ("t" + std::to_string(p.texture) + "." + "xyzw"[p.channel] + " s" + std::to_string(p.sampler)).c_str(),
            kBakeClassNames[static_cast<size_t>(bake)], varying.empty() ? "" : (" (" + varying + ")").c_str());
    }
    return 0;
}
