// The Mac's reference tracer: traces a saved frame again with the compute shader's own rays and traversal
// (shaders/trace.hlsli compiled as C++ in core/kernel.cpp) and writes reference.f32 beside history.f32, with each pixel's
// samples squared and summed in reference-squares.f32, for check_trace.py to compare.
//
//   trace_reference <trace folder> [scene folder]
//
// The trace folder is what PhotorealSaveTrace wrote; the scene folder holds the trace-scene.bin trace.tsv names, by
// default beside the trace folder. It builds the scene's BVH with core/bvh.cpp and checks its hash against the Deck's,
// then gives every pixel the samples the Deck's history says it has, from the same sequence, with the constants the
// Deck traced with and the scene's alpha tests, on all the Mac's cores. Then it renders the view's depth from the scene,
// a ray from the eye to every pixel with depth but grass, with the alpha tests and with whole cards, and reports how much
// of it agrees with the depth buffer, as check_scene.py's check 2 judges a face's meshes. Prints PASS, FAIL and INFO
// lines like the tests.
#include "../core/trace_files.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
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
    // Within trace.tsv's digits: 17 since T1b, 9 before, so a 9-digit reference rounds at 5e-9 of its value.
    bool same_reference = true;
    for (int k = 0; k < 3; k++)
        same_reference &= std::fabs(scene->reference[k] - save->reference[k]) <= 1e-8 * std::max(1.0, std::fabs(scene->reference[k]));
    check("the scene's reference is the one the frame was traced with", same_reference);

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
    // Each pixel's occlusion and sun samples squared and summed, so check_trace.py knows each pixel's sampling variance.
    std::vector<float> squares(size_t(width) * height * 2, 0.0f);
    const std::vector<uint32_t> *alpha = scene->alpha.empty() ? nullptr : &scene->alpha;
    std::printf("INFO the scene holds %s\n", alpha ? (std::to_string(scene->alpha.size() * 4) + " bytes of alpha tests").c_str() : "no alpha tests");
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
                    double square[2] = {};
                    for (uint32_t k = std::max(constants.trace_counts[0], constants.trace_counts[1]); k > 0; k--)
                    {
                        const PixelTrace r = trace_pixel(bvh, image, constants, x, y, h, alpha);
                        if (!r.world)
                            break;
                        for (int i = 0; i < 2; i++)
                            if (r.history[2 + i] > h[2 + i])
                                square[i] += double(r.history[i] - h[i]) * (r.history[i] - h[i]);
                        h = r.history;
                        my_rays += r.counters[static_cast<size_t>(TraceCounter::rays)];
                        my_guard += r.counters[static_cast<size_t>(TraceCounter::guard)];
                        my_overflow += r.counters[static_cast<size_t>(TraceCounter::overflow)];
                    }
                    std::copy(h.begin(), h.end(), reference.begin() + static_cast<std::ptrdiff_t>(at));
                    squares[at / 2] = static_cast<float>(square[0]);
                    squares[at / 2 + 1] = static_cast<float>(square[1]);
                }
            rays += my_rays;
            guard += my_guard;
            overflow += my_overflow;
        });
    for (std::thread &t : threads)
        t.join();
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    std::printf("INFO traced %llu rays in %.1f s on %u threads (%.2f Mrays/s); %llu cut short by a cap, %llu stack overflows\n",
        static_cast<unsigned long long>(rays.load()), seconds, options.tasks, rays.load() / std::max(seconds, 1e-9) / 1e6,
        static_cast<unsigned long long>(guard.load()), static_cast<unsigned long long>(overflow.load()));
    std::ofstream out(folder / "reference.f32", std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char *>(reference.data()), static_cast<std::streamsize>(reference.size() * sizeof(float)));
    std::ofstream out_squares(folder / "reference-squares.f32", std::ios::binary | std::ios::trunc);
    out_squares.write(reinterpret_cast<const char *>(squares.data()), static_cast<std::streamsize>(squares.size() * sizeof(float)));
    check("reference.f32 and reference-squares.f32 are written beside history.f32", static_cast<bool>(out) && static_cast<bool>(out_squares));

    // The view's depth: each pixel's surface from the stored depth through the traced frame's camera, as trace.hlsli
    // reconstructs it, and a ray from the eye toward it against every caster class.
    const TraceConstants &c = save->constants;
    const Vec3f eye = { c.eye_position[0], c.eye_position[1], c.eye_position[2] };
    // Agreeing pixels with the alpha tests and with whole cards, and pixels, for stencil 128 and 144, 136 and 137, and the rest.
    std::atomic<uint64_t> agree[2][3] {}, pixels[3] {};
    next_row = 0;
    threads.clear();
    for (uint32_t t = 0; t < options.tasks; t++)
        threads.emplace_back([&] {
            uint64_t my_agree[2][3] {}, my_pixels[3] {};
            for (uint32_t y; (y = next_row++) < height;)
                for (uint32_t x = 0; x < width; x++)
                {
                    const size_t at = size_t(y) * width + x;
                    const uint8_t stencil = save->stencil[at];
                    const float depth = save->depth[at];
                    if (!(depth > 0) || stencil == 129)
                        continue;
                    float u = (x + 0.5f) / width, v = (y + 0.5f) / height;
                    if (c.trace_render[2] != 0)
                        v = 1 - v;
                    const float clip[4] = { u * 2 - 1, 1 - v * 2, depth, 1 };
                    float h[4] = {};
                    for (int r = 0; r < 4; r++)
                        for (int k = 0; k < 4; k++)
                            h[r] += c.clip_rows[r][k] * clip[k];
                    const Vec3f d = { h[0] / h[3] - eye[0], h[1] / h[3] - eye[1], h[2] / h[3] - eye[2] };
                    const float distance = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
                    if (!(distance > 0))
                        continue;
                    const Vec3f direction = { d[0] / distance, d[1] / distance, d[2] / distance };
                    const int group = stencil == 128 || stencil == 144 ? 0 : stencil == 136 || stencil == 137 ? 1 : 2;
                    my_pixels[group]++;
                    for (int tested = 0; tested < 2; tested++)
                    {
                        const std::optional<Hit> hit = trace_nearest(bvh, eye, direction, distance * 1.5f + 1, tested == 0 ? kAllClasses | kAlphaTests : kAllClasses,
                            nullptr, alpha);
                        my_agree[tested][group] += hit && std::fabs(hit->t - distance) <= 0.02f * distance;
                    }
                }
            for (int g = 0; g < 3; g++)
            {
                pixels[g] += my_pixels[g];
                for (int tested = 0; tested < 2; tested++)
                    agree[tested][g] += my_agree[tested][g];
            }
        });
    for (std::thread &t : threads)
        t.join();
    const auto share = [&](int tested, std::initializer_list<int> groups) {
        uint64_t a = 0, n = 0;
        for (int g : groups)
        {
            a += agree[tested][g];
            n += pixels[g];
        }
        char text[64];
        std::snprintf(text, sizeof text, "%.4f of %llu", n ? double(a) / n : 0.0, static_cast<unsigned long long>(n));
        return std::pair { std::string(text), n ? double(a) / n : 0.0 };
    };
    std::printf("INFO the view's depth from the scene within 2%% of the depth buffer, with the alpha tests: %s pixels with depth but grass "
                "(stencil 128 and 144 %s, 136 and 137 %s); with whole cards: %s (%s, %s)\n",
        share(0, { 0, 1, 2 }).first.c_str(), share(0, { 0 }).first.c_str(), share(0, { 1 }).first.c_str(), share(1, { 0, 1, 2 }).first.c_str(),
        share(1, { 0 }).first.c_str(), share(1, { 1 }).first.c_str());
    if (alpha)
        check("the alpha tests do not lower the view's depth agreement", share(0, { 0, 1, 2 }).second >= share(1, { 0, 1, 2 }).second);
    std::printf("%s: %d failed\n", __FILE__, failures);
    return failures == 0 ? 0 : 1;
}
