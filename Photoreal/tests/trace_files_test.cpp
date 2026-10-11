// The trace folder PhotorealSaveTrace writes: its files and their sizes, trace.tsv's rows, and reading it back as the Mac's
// reference tracer does.
#include "../core/trace_files.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

using namespace photoreal;

namespace
{
    int failures = 0;

    void check(const char *name, bool ok)
    {
        std::printf("%s %s\n", ok ? "PASS" : "FAIL", name);
        failures += ok ? 0 : 1;
    }

    std::string slurp(const std::filesystem::path &path)
    {
        std::ifstream file(path, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    }
}

int main(int argc, char **argv)
{
    TraceSave s;
    s.size = { 3, 2 };
    s.history = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24 };
    s.depth = { 0.5f, 0.25f, 0, 0, 0.125f, 1 };
    s.stencil = { 128, 128, 0, 133, 137, 129 };
    s.normals = { 1, 2, 3, 4, 5, 6 };
    s.mask.assign(12, 200);
    s.irradiance.assign(24, 7);
    s.constants.eye_position[1] = 1.5f;
    s.constants.sun_cone[1] = 1;
    s.constants.trace_tiles[3] = 3;
    s.settings.ao_samples = 16;
    s.level = 0.6f;
    s.camera.world_to_view[0] = 1;
    s.reference = { 1648.5, 229.75, 9468.25 };
    s.sun.emplace();
    s.sun->fill(9);
    s.sun_source = "game";
    s.floating_origin = Vec3 { 1648.686f, 0, 9468.529f };
    s.scene = "scene-20261011-194351";
    s.generation = 3;
    s.bvh_hash = 0xb090ff09807266caull;
    s.triangles = 2377274;
    s.nodes = 756325;
    s.passes = 12;
    s.counters = { 1000, 1, 2, 30, 1 };
    s.mrays = 12.5;
    s.frame = 4321;
    const std::string tsv = trace_tsv(s);
    check("trace.tsv names the scene, the BVH's hash, the samples and counters, the frame of the scene and the settings",
        tsv.find("key\tvalue\nwidth\t3\nheight\t2\nframe\t4321\nscene\tscene-20261011-194351\ngeneration\t3\nbvh_hash\tb090ff09807266ca\n"
                 "triangles\t2377274\nnodes\t756325\npasses\t12\nrays\t1000\nguard_stops\t1\nstack_overflows\t2\ngrid_tested\t30\ngrid_off\t1\n"
                 "mrays\t12.5\nreference\t1648.5,229.75,9468.25\nfloating_origin\t1648.68604,0,9468.5293\nsun_source\tgame\ntoward_sun\t0,1,0\n"
                 "eye\t0,1.5,0\nworld_to_view\t1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0\n") == 0
            && tsv.find("trace.ao_samples\t16\n") != std::string::npos && tsv.find("ambient.level\t0.600000024\n") != std::string::npos);
    if (argc > 1)
    {
        const std::filesystem::path folder = std::filesystem::path(argv[1]) / "trace-roundtrip";
        const std::vector<std::string> failed = write_trace_folder(folder, s);
        check("the folder's files hold the save's bytes", failed.empty() && std::filesystem::file_size(folder / "history.f32") == 96
            && std::filesystem::file_size(folder / "depth.r32") == 24 && std::filesystem::file_size(folder / "stencil.r8") == 6
            && std::filesystem::file_size(folder / "normals.r10g10b10a2") == 24 && std::filesystem::file_size(folder / "mask.r8g8") == 12
            && std::filesystem::file_size(folder / "irradiance.r11g11b10") == 24 && std::filesystem::file_size(folder / "constants.bin") == 160
            && std::filesystem::file_size(folder / "sun.bin") == kSunCbufferTotal && slurp(folder / "trace.tsv") == tsv);
        std::string why;
        const std::optional<TraceSave> back = read_trace_folder(folder, why);
        check("and read back as the reference tracer needs them", back && back->size == s.size && back->history == s.history && back->depth == s.depth
            && back->stencil == s.stencil && back->normals == s.normals && back->mask == s.mask && back->scene == s.scene && back->bvh_hash == s.bvh_hash
            && back->passes == 12 && back->generation == 3 && back->reference == s.reference && back->constants.eye_position[1] == 1.5f && back->sun
            && (*back->sun)[5] == 9);
        std::filesystem::remove(folder / "depth.r32");
        check("a missing image says which files it needs", !read_trace_folder(folder, why) && why.find("depth.r32") != std::string::npos);
    }
    std::printf("%s: %d failed\n", __FILE__, failures);
    return failures == 0 ? 0 : 1;
}
