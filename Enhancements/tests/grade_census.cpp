// Prints the grade layout core/grade.cpp finds in each shader given, for check_grade.sh.
#include "../core/grade.h"

#include <cstdio>
#include <fstream>
#include <iterator>
#include <vector>

using namespace enhancements;

int main(int argc, char **argv)
{
    int found = 0;
    for (int i = 1; i < argc; i++)
    {
        std::ifstream file(argv[i], std::ios::binary);
        const std::vector<uint8_t> code((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        const std::optional<Shex> shex = decode_shex(code.data(), code.size());
        const std::optional<GradeLayout> layout = shex ? find_grade(*shex) : std::nullopt;
        if (!layout)
            continue;
        found++;
        std::printf("%s color %u exposure %u.%c gamma %u.%c bloom ", argv[i], layout->color_row, layout->exposure.row, "xyzw"[layout->exposure.component],
            layout->gamma.row, "xyzw"[layout->gamma.component]);
        if (layout->bloom)
            std::printf("%u.%c\n", layout->bloom->row, "xyzw"[layout->bloom->component]);
        else
            std::printf("none\n");
    }
    std::printf("%d of %d shaders have a grade layout\n", found, argc - 1);
    return 0;
}
