// Bakes the alpha programs of saved traced scenes into coverage masks (core/alpha_bake.cpp), as the add-on does before
// it builds the BVH, and prints how each scene's alpha-tested triangles, draws and programs baked: exactly,
// approximately or not at all (whole cards), with the bake's time, and per program its draws and triangles by class.
// Then it checks the masks against the programs: at 16 random points of every record's triangle, the mask's verdict
// against the program's own (alpha_keeps), by class. A tool, not part of run.sh, since the scenes live in .work.
//
//   c++ -std=c++17 -O2 -pthread -I .. bake_scene.cpp ../core/*.cpp -o bake_scene
//   ./bake_scene [--tasks N] <scene folder or trace-scene.bin>...
#include "../core/alpha_bake.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <random>
#include <string>

using namespace photoreal;
namespace fs = std::filesystem;

int main(int argc, char **argv)
{
    uint32_t tasks = 3;
    int status = 0;
    for (int i = 1; i < argc; i++)
    {
        if (std::strcmp(argv[i], "--tasks") == 0 && i + 1 < argc)
        {
            tasks = static_cast<uint32_t>(std::stoul(argv[++i]));
            continue;
        }
        fs::path path = argv[i];
        if (fs::is_directory(path))
            path /= "trace-scene.bin";
        std::ifstream file(path, std::ios::binary);
        const std::string bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        std::optional<TraceSceneFile> scene = parse_trace_scene(bytes);
        if (!scene)
        {
            std::printf("%s: not a trace-scene.bin\n", path.u8string().c_str());
            status = 1;
            continue;
        }
        const std::vector<TraceTriangle> before = scene->triangles;
        const auto start = std::chrono::steady_clock::now();
        const AlphaMasks masks = bake_alpha(scene->alpha, scene->triangles, tasks);
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        const AlphaBakeCounts &c = masks.counts;
        const auto by = [](const auto &a) {
            return std::to_string(a[0]) + " exact, " + std::to_string(a[1]) + " approximate, " + std::to_string(a[2]) + " fallback";
        };
        std::printf("%s: %zu triangles, %zu bytes of alpha programs; baked in %.2f s on %u threads into %zu bytes (%u records, %u masks)\n",
            path.parent_path().filename().u8string().c_str(), scene->triangles.size(), scene->alpha.size() * 4, seconds, tasks,
            masks.words.size() * 4, c.records, c.masks);
        std::printf("  triangles: %s; %llu kept everywhere (now whole), %llu discarded everywhere, %llu past the records' cap%s%s\n", by(c.triangles).c_str(),
            static_cast<unsigned long long>(c.always), static_cast<unsigned long long>(c.never), static_cast<unsigned long long>(c.past_max),
            c.why.empty() ? "" : "; first fallback: ", c.why.c_str());
        std::printf("  draws by their worst triangle: %s\n  programs by their worst draw: %s\n", by(c.draws).c_str(), by(c.programs).c_str());
        // Per program: its instructions and floats, its draws and their triangles by class, and fallback reasons.
        struct Program
        {
            uint32_t draws = 0;
            std::array<uint64_t, 3> triangles {};
            std::map<std::string, uint32_t> why;
        };
        std::map<uint32_t, Program> programs;
        for (const BakeDraw &d : masks.draws)
        {
            Program &p = programs[d.program];
            p.draws++;
            for (size_t k = 0; k < 3; k++)
                p.triangles[k] += d.triangles[k];
            if (!d.why.empty())
                p.why[d.why]++;
        }
        for (const auto &[offset, p] : programs)
        {
            const uint32_t at = scene->alpha[1] + offset;
            std::string why;
            for (const auto &[w, n] : p.why)
                why += "; " + std::to_string(n) + " with " + w;
            std::printf("  program at %u: %u instructions, %u floats; %u draws; triangles %s%s\n", offset, scene->alpha[at] & 0xFFFF,
                scene->alpha[at + 1] & 0xFFFF, p.draws, by(p.triangles).c_str(), why.c_str());
        }
        // Each record's verdicts against its program's, by its triangle's class: exact when the verdict reads no input
        // that varies over the triangle.
        std::mt19937 random(1);
        std::uniform_real_distribution<float> unit(0, 1);
        uint64_t same[2] = {}, points[2] = {}, kept[2] = {};
        for (size_t t = 0; t < scene->triangles.size(); t++)
        {
            const uint32_t record = scene->triangles[t].info & kOpaque;
            if (record == kOpaque)
                continue;
            const uint32_t source = before[t].info & kOpaque;
            const int cls = masks.classes[record] == BakeClass::exact ? 0 : 1;
            for (int k = 0; k < 16; k++)
            {
                float u = unit(random), v = unit(random);
                if (u + v > 1)
                {
                    u = 1 - u;
                    v = 1 - v;
                }
                const bool program = alpha_keeps(scene->alpha, source, u, v), mask = alpha_mask_keeps(masks.words, record, u, v);
                same[cls] += program == mask;
                kept[cls] += program;
                points[cls]++;
            }
        }
        for (int k = 0; k < 2; k++)
            std::printf("  %s records' masks against their programs at 16 random points each: %llu of %llu agree (%.5f); the programs keep %.4f\n",
                k == 0 ? "exact" : "approximate", static_cast<unsigned long long>(same[k]), static_cast<unsigned long long>(points[k]),
                points[k] ? double(same[k]) / points[k] : 1.0, points[k] ? double(kept[k]) / points[k] : 0.0);
    }
    return status;
}
