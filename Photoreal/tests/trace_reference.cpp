// The Mac's reference tracer: traces a saved frame again with the compute shader's own rays and traversal
// (shaders/trace.hlsli compiled as C++ in core/kernel.cpp) and writes reference.f32 beside history.f32, for
// check_trace.py to compare.
//
//   trace_reference <trace folder> [scene folder]
//
// The trace folder is what PhotorealSaveTrace wrote; the scene folder holds the trace-scene.bin trace.tsv names, by
// default beside the trace folder. It builds the scene's BVH with core/bvh.cpp and checks its hash against the Deck's,
// then gives every pixel the samples the Deck's history says it has, from the same sequence, with the constants the
// Deck traced with, on all the Mac's cores. Prints PASS, FAIL and INFO lines like the tests.
#include "../core/trace_files.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

using namespace photoreal;
namespace fs = std::filesystem;

namespace
{
    int failures = 0;

    void check(const std::string &name, bool ok)
    {
        std::printf("%s %s\n", ok ? "PASS" : "FAIL", name.c_str());
        failures += ok ? 0 : 1;
    }
}

int main(int argc, char **argv)
{
    if (argc < 2)
    {
        std::fprintf(stderr, "usage: trace_reference <trace folder> [scene folder]\n");
        return 2;
    }
    const fs::path folder = argv[1];
    std::string why;
    const std::optional<TraceSave> save = read_trace_folder(folder, why);
    if (!save)
    {
        std::printf("FAIL %s: %s\n", folder.u8string().c_str(), why.c_str());
        return 1;
    }
    const fs::path scene_folder = argc > 2 ? fs::path(argv[2]) : folder.parent_path() / save->scene;
    std::ifstream file(scene_folder / "trace-scene.bin", std::ios::binary);
    const std::string bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    const std::optional<TraceSceneFile> scene = parse_trace_scene(bytes);
    if (!scene)
    {
        std::printf("FAIL %s holds no trace-scene.bin\n", scene_folder.u8string().c_str());
        return 1;
    }
    check("the scene's reference is the one the frame was traced with", scene->reference == save->reference);

    BvhOptions options;
    options.tasks = std::max(1u, std::thread::hardware_concurrency());
    auto start = std::chrono::steady_clock::now();
    const Bvh bvh = build_bvh(scene->triangles, options);
    const double build = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    char line[256];
    std::snprintf(line, sizeof line, "the Mac's BVH of the %zu triangles, built in %.2f s, has the Deck's hash %016llx", scene->triangles.size(), build,
        static_cast<unsigned long long>(save->bvh_hash));
    check(line, bvh.hash == save->bvh_hash);

    TraceImage image;
    image.size = save->size;
    image.depth = save->depth;
    image.normals = save->normals;
    image.stencil = save->stencil;
    const uint32_t width = save->size.width, height = save->size.height;
    std::vector<float> reference(size_t(width) * height * 4, 0.0f);
    std::atomic<uint32_t> next_row { 0 };
    std::atomic<uint64_t> rays { 0 }, guard { 0 }, overflow { 0 };
    start = std::chrono::steady_clock::now();
    std::vector<std::thread> threads;
    for (uint32_t t = 0; t < options.tasks; t++)
        threads.emplace_back([&] {
            uint64_t my_rays = 0, my_guard = 0, my_overflow = 0;
            for (uint32_t y; (y = next_row++) < height;)
                for (uint32_t x = 0; x < width; x++)
                {
                    const size_t at = (size_t(y) * width + x) * 4;
                    // The Deck's counts become this pixel's caps, so it gets exactly their samples; no consistency ray.
                    TraceConstants constants = save->constants;
                    constants.trace_counts[0] = static_cast<uint32_t>(save->history[at + 2]);
                    constants.trace_counts[1] = static_cast<uint32_t>(save->history[at + 3]);
                    constants.trace_render[3] = 0;
                    std::array<float, 4> h {};
                    for (uint32_t k = std::max(constants.trace_counts[0], constants.trace_counts[1]); k > 0; k--)
                    {
                        const PixelTrace r = trace_pixel(bvh, image, constants, x, y, h);
                        if (!r.world)
                            break;
                        h = r.history;
                        my_rays += r.counters[static_cast<size_t>(TraceCounter::rays)];
                        my_guard += r.counters[static_cast<size_t>(TraceCounter::guard)];
                        my_overflow += r.counters[static_cast<size_t>(TraceCounter::overflow)];
                    }
                    std::copy(h.begin(), h.end(), reference.begin() + static_cast<std::ptrdiff_t>(at));
                }
            rays += my_rays;
            guard += my_guard;
            overflow += my_overflow;
        });
    for (std::thread &t : threads)
        t.join();
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    std::printf("INFO traced %llu rays in %.1f s on %u threads (%.2f Mrays/s); %llu stopped by the step guard, %llu stack overflows\n",
        static_cast<unsigned long long>(rays.load()), seconds, options.tasks, rays.load() / std::max(seconds, 1e-9) / 1e6,
        static_cast<unsigned long long>(guard.load()), static_cast<unsigned long long>(overflow.load()));
    std::ofstream out(folder / "reference.f32", std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char *>(reference.data()), static_cast<std::streamsize>(reference.size() * sizeof(float)));
    check("reference.f32 is written beside history.f32", static_cast<bool>(out));
    std::printf("%s: %d failed\n", __FILE__, failures);
    return failures == 0 ? 0 : 1;
}
