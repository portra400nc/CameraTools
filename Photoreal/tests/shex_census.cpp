// Census tool for core/shex.* and core/alpha.*: decodes FrameCensus shader files (<capture>/shaders/<hash>.<stage>.dxbc).
//
//   shex_census names <dxbc>...   one line per shader: its path, then each instruction's name, declarations left out
//
// check_shex.sh runs it over the census captures; it is not part of run.sh, since it needs .work.
#include "../core/shex.h"

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
    if (argc < 2 || std::strcmp(argv[1], "names") != 0)
    {
        std::fprintf(stderr, "usage: shex_census names <dxbc>...\n");
        return 2;
    }
    for (int i = 2; i < argc; i++)
    {
        const std::string b = bytes_of(argv[i]);
        const auto *code = reinterpret_cast<const uint8_t *>(b.data());
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
        }
    }
    return 0;
}
