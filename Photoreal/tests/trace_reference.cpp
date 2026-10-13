// The Mac's reference tracer: traces a saved frame again with the compute shaders' own rays and traversal
// (shaders/trace.hlsli and shaders/gather.hlsli compiled as C++ in core/kernel.cpp) and writes reference.f32 beside
// history.f32, with each pixel's samples squared and summed in reference-squares.f32, for check_trace.py to compare.
//
//   trace_reference <trace folder> [scene folder] [--leaf-cards <ps.dxbc>] [--drop-class <class>] [--drop-near <meters>] [--gather]
//
// A save that gathered is traced as the add-on traced it: the trace kernel for the sun alone, and the gather kernel for
// the occlusion and the gathered light, with the gather's constants it saved and the light of the scene folder's
// trace-light.bin. Then reference-gather-hit.f32 and reference-gather-sky.f32 hold the Mac's gather sums, and
// reference-gather-squares.f32 three floats a pixel: its hits' and its skies' luminances squared and summed, and the
// luminance of its hits the sun did not light, summed. --gather does the same for a
// save of T1, before the gather existed, whose occlusion samples are the gather's first samples, ray for ray: its light
// stands in where the save and the scene lack it (the scene's own trace-light.bin when it has one; else the albedo bake's
// sources from the faces' depth, with any surface within 1.5 m in front of a sample covering it, and the census's albedos
// by class; a sky map from the faces' pictures through the game's tone curve inverted; the ambient fit of the save's own
// irradiance; and the sun's color of census capture 111824 along the pushed camera's sun), says so, and writes them as
// reference-light.bin, in trace-light.bin's format, for predict_shot.py.
//
// The trace folder is what PhotorealSaveTrace wrote; the scene folder holds the trace-scene.bin trace.tsv names, by
// default beside the trace folder. It bakes the scene's alpha programs into masks as the add-on does
// (core/alpha_bake.cpp), builds the scene's BVH with core/bvh.cpp and checks its hash against the Deck's, then gives
// every pixel the samples the Deck's history says it has, from the same sequence, with the constants the Deck traced
// with and the scene's alpha tests, on all the Mac's cores. --leaf-cards gives every vegetation and foliage card of a
// scene without alpha tests the census program in ps.dxbc and leaf-shaped blobs about 43% opaque, as alpha_test.cpp's
// fixture does, so the Mac's rays a second with and without alpha tests compare their cost; the BVH's hash then differs
// from the Deck's and is not checked. --drop-class leaves a class out of the scene, to measure what its triangles cost the
// traversal. --drop-near leaves out every triangle with a corner within that distance of the scene's reference, the
// capture's camera: the scenes of builds 205 to 207 hold their equal-depth draws misread, fans from one real corner
// to two beside the camera (README, "Misread equal-depth draws"), which the scene build now leaves out, so
// --drop-near 0.5 traces such a scene as it would be built today. Neither scene is the Deck's, so the hash is not
// checked and check_trace.py's comparison with the Deck's history means nothing. It fails when more than 0.1% of the rays
// were cut short by a cap. Then it renders the view's depth from the scene, a ray from the eye to every pixel with depth
// but grass, with the alpha tests and with whole cards, and reports how much of it agrees with the depth buffer, as
// check_scene.py's check 2 judges a face's meshes, and how many of those rays a cap cut short, which says the scene
// holds something no scene should: a ray cut short stops wherever its work ran out. Prints PASS, FAIL and INFO lines
// like the tests.
#include "../core/alpha_bake.h"
#include "../core/trace_files.h"
#include "scene_light.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <mutex>
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
    std::vector<std::string> args(argv + 1, argv + argc);
    std::string leaf_cards, drop, drop_near;
    bool want_gather = false;
    for (size_t i = 0; i < args.size();)
        if (args[i] == "--gather")
        {
            want_gather = true;
            args.erase(args.begin() + static_cast<std::ptrdiff_t>(i));
        }
        else if (i + 1 < args.size() && (args[i] == "--leaf-cards" || args[i] == "--drop-class" || args[i] == "--drop-near"))
        {
            (args[i] == "--leaf-cards" ? leaf_cards : args[i] == "--drop-class" ? drop : drop_near) = args[i + 1];
            args.erase(args.begin() + static_cast<std::ptrdiff_t>(i), args.begin() + static_cast<std::ptrdiff_t>(i) + 2);
        }
        else
            i++;
    const float radius = drop_near.empty() ? 0.0f : std::strtof(drop_near.c_str(), nullptr);
    if (args.empty() || args[0].rfind("--", 0) == 0 || (!drop_near.empty() && !(radius > 0)))
    {
        std::fprintf(stderr, "usage: trace_reference <trace folder> [scene folder] [--leaf-cards <ps.dxbc>] [--drop-class <class>] [--drop-near <meters>] [--gather]\n");
        return 2;
    }
    const fs::path folder = args[0];
    std::string why;
    const std::optional<TraceSave> save = read_trace_folder(folder, why);
    if (!save)
    {
        std::printf("FAIL %s: %s\n", folder.u8string().c_str(), why.c_str());
        return 1;
    }
    const fs::path scene_folder = args.size() > 1 ? fs::path(args[1]) : folder.parent_path() / save->scene;
    std::ifstream file(scene_folder / "trace-scene.bin", std::ios::binary);
    const std::string bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    std::optional<TraceSceneFile> scene = parse_trace_scene(bytes);
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

    // The scene's light, when it was captured with one. The add-on bakes each triangle's light word into the triangles
    // before it builds their BVH, so the Deck's hash covers the words: they go in here, before the build, too.
    std::optional<LightFile> light = parse_light(scene_light::slurp(scene_folder / "trace-light.bin"));
    if (light && light->words.size() != scene->triangles.size())
        light.reset();
    if (light)
        for (size_t i = 0; i < scene->triangles.size(); i++)
            scene->triangles[i].light = light->words[i];

    // A class left out of the scene, to measure what its triangles cost the traversal: casters bits skip a class at its
    // leaves but still walk its nodes, while a scene without it does not.
    if (!drop.empty())
    {
        const auto named = std::find(kTraceClassNames.begin(), kTraceClassNames.end(), drop);
        if (named == kTraceClassNames.end())
        {
            std::printf("FAIL --drop-class %s names no class\n", drop.c_str());
            return 1;
        }
        const TraceClass dropped = static_cast<TraceClass>(named - kTraceClassNames.begin());
        const size_t before = scene->triangles.size();
        scene->triangles.erase(std::remove_if(scene->triangles.begin(), scene->triangles.end(),
            [dropped](const TraceTriangle &t) { return info_class(t.info) == dropped; }), scene->triangles.end());
        std::printf("INFO --drop-class %s: %zu of %zu triangles left out\n", drop.c_str(), before - scene->triangles.size(), before);
    }
    if (radius > 0)
    {
        const size_t before = scene->triangles.size();
        scene->triangles.erase(std::remove_if(scene->triangles.begin(), scene->triangles.end(), [radius](const TraceTriangle &t) {
            return std::any_of(t.corner.begin(), t.corner.end(), [radius](const Vec3f &c) { return std::hypot(c[0], c[1], c[2]) < radius; });
        }), scene->triangles.end());
        std::printf("INFO --drop-near %g: %zu of %zu triangles left out\n", radius, before - scene->triangles.size(), before);
    }
    if (!leaf_cards.empty() && scene->alpha.empty())
    {
        std::ifstream code_file(leaf_cards, std::ios::binary);
        const std::string code((std::istreambuf_iterator<char>(code_file)), std::istreambuf_iterator<char>());
        const AlphaAnalysis analysis = analyze_alpha(reinterpret_cast<const uint8_t *>(code.data()), code.size());
        if (!analysis.program || analysis.program->attribute_floats() != 2)
        {
            std::printf("FAIL %s holds no alpha program of one UV\n", leaf_cards.c_str());
            return 1;
        }
        // Its cutoff row's .y at 0.5, as 68b05754 reads cb2[14].y; the blobs of alpha_test.cpp's fixture.
        AlphaBuffer buffer;
        const float half = 0.5f;
        uint32_t half_bits;
        std::memcpy(&half_bits, &half, 4);
        const uint32_t draw = buffer.add_draw(buffer.add_program(analysis.program), 0, 1 | 1 << 3, { 0, half_bits, 0, 0 });
        std::vector<uint8_t> atlas(kAtlasTile * kAtlasTile);
        for (uint32_t i = 0; i < atlas.size(); i++)
            atlas[i] = ((i % kAtlasTile / 16) * 5 + (i / kAtlasTile / 16) * 3) % 7 < 3 ? 255 : 0;
        const float uv[6] = { 0, 0, 1, 0, 0, 1 };
        uint32_t cards = 0;
        for (TraceTriangle &t : scene->triangles)
            if (info_class(t.info) == TraceClass::vegetation || info_class(t.info) == TraceClass::foliage)
            {
                t.info = triangle_info(info_class(t.info), buffer.add_record(draw, uv, 2));
                cards++;
            }
        scene->alpha = buffer.words(atlas);
        std::printf("INFO --leaf-cards: %u vegetation and foliage cards alpha-tested by %s with leaf-shaped blobs\n", cards, leaf_cards.c_str());
    }

    BvhOptions options;
    options.tasks = std::max(1u, std::thread::hardware_concurrency());
    const size_t program_bytes = scene->alpha.size() * 4;
    auto start = std::chrono::steady_clock::now();
    const AlphaMasks masks = scene->alpha.empty() ? AlphaMasks {} : bake_alpha(scene->alpha, scene->triangles, options.tasks);
    const double bake = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    if (program_bytes != 0)
    {
        const AlphaBakeCounts &b = masks.counts;
        std::printf("INFO %zu bytes of alpha programs baked in %.2f s into %zu bytes of masks: %u records and %u masks; triangles %llu exact, %llu approximate, "
                    "%llu whole as they cannot be baked, %llu whole as their test keeps every point\n",
            program_bytes, bake, masks.words.size() * 4, b.records, b.masks, static_cast<unsigned long long>(b.triangles[0]),
            static_cast<unsigned long long>(b.triangles[1]), static_cast<unsigned long long>(b.triangles[2]), static_cast<unsigned long long>(b.always));
    }
    start = std::chrono::steady_clock::now();
    const Bvh bvh = build_bvh(scene->triangles, options);
    const double build = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    char line[256];
    std::snprintf(line, sizeof line, "the Mac's BVH of the %zu triangles, built in %.2f s, has the Deck's hash %016llx", scene->triangles.size(), build,
        static_cast<unsigned long long>(save->bvh_hash));
    const bool decks_scene = leaf_cards.empty() && drop.empty() && drop_near.empty();
    if (decks_scene)
        check(line, bvh.hash == save->bvh_hash);
    else
        std::printf("INFO the Mac's BVH of the %zu triangles, not the Deck's scene, built in %.2f s, has its own hash %016llx\n", scene->triangles.size(),
            build, static_cast<unsigned long long>(bvh.hash));

    TraceImage image;
    image.size = save->size;
    image.depth = save->depth;
    image.normals = save->normals;
    image.stencil = save->stencil;
    const uint32_t width = save->size.width, height = save->size.height;

    // The gather's light: its constants and the scene's sky map; the triangles' light words are in the BVH already, but
    // for a scene captured without light, whose stand-ins go into a copy of it.
    const bool gathering = save->gathering || want_gather;
    GatherConstants gather = save->gather_constants;
    std::vector<float> sky;
    std::array<uint64_t, 4> hits_by_source {};
    Bvh lit_copy;
    if (gathering)
    {
        if (save->gathering && !light)
        {
            std::printf("FAIL the save gathered, but %s holds no trace-light.bin of its %zu triangles\n", scene_folder.u8string().c_str(), scene->triangles.size());
            return 1;
        }
        if (light)
        {
            sky = light->light.sky.texels;
            std::printf("INFO the light of %s's trace-light.bin: %u real sky texels%s, the ambient %s, the sun's color %s and direction %s\n",
                scene_folder.u8string().c_str(), light->light.sky.real, light->light.sky.fallback ? " (it fell back)" : "",
                light->light.ambient.kind == AmbientFit::Kind::fit ? "fit" : "not fit", kLightSourceNames[static_cast<size_t>(light->light.sun.color_source)],
                kLightSourceNames[static_cast<size_t>(light->light.sun.shade_source)]);
        }
        else
        {
            // Stand-ins for a capture made before PHOTOREAL_SCENE_LIGHT.
            const scene_light::SceneFaces faces = scene_light::read_faces(scene_folder, std::nullopt, true);
            // The faces' depth at their full size, an albedo of no consequence, and any surface covering: what the bake can
            // tell of where its samples lie without the copies.
            std::vector<FaceImages> bake = scene_light::read_faces(scene_folder, std::nullopt, false).faces;
            for (FaceImages &f : bake)
            {
                f.albedo.assign(f.depth.size() * 4, 128);
                f.stencil.assign(f.depth.size(), 136);
            }
            std::vector<uint32_t> words;
            std::vector<TraceTriangle> triangles = scene->triangles;
            const auto start_bake = std::chrono::steady_clock::now();
            const AlbedoCounts counts = bake_albedo(triangles, {}, bake, scene->reference, options.tasks);
            std::printf("STANDIN no trace-light.bin: the albedo bake's sources from the depth of %zu faces, with any surface within 1.5 m in front covering, "
                        "in %.2f s: %llu direct, %llu covered, %llu by class (no materials); albedos the census's by class\n",
                bake.size(), std::chrono::duration<double>(std::chrono::steady_clock::now() - start_bake).count(),
                static_cast<unsigned long long>(counts.triangles[0]), static_cast<unsigned long long>(counts.triangles[1]),
                static_cast<unsigned long long>(counts.triangles[2] + counts.triangles[3]));
            words.resize(triangles.size());
            for (size_t i = 0; i < triangles.size(); i++)
                words[i] = pack_light(scene_light::class_albedo(info_class(triangles[i].info)), light_source(triangles[i].light));
            const SkyMap map = build_sky_map(faces.faces, { 0.62f, 0.84f, 1.07f });
            sky = map.texels;
            std::printf("STANDIN the sky map from the faces' pictures through the game's tone curve inverted: %u real texels of %u%s\n", map.real,
                kSkyMapSide * kSkyMapSide, map.fallback ? ", fell back to census 111824's horizon sky" : "");
            const AmbientFit fit = fit_ambient(save->size, save->irradiance, std::vector<uint8_t>(reinterpret_cast<const uint8_t *>(save->normals.data()),
                reinterpret_cast<const uint8_t *>(save->normals.data()) + save->normals.size() * 4), save->stencil, save->depth, { 0.33f, 0.59f, 0.81f });
            SunLight sun;
            sun.color = { 2.804184f, 2.501149f, 1.130967f };
            sun.shade = { save->camera.toward_sun[0], save->camera.toward_sun[1], save->camera.toward_sun[2] };
            GatherSettings settings;
            settings.samples = 0;
            for (size_t i = 0; i < save->history.size(); i += 4)
                settings.samples = std::max(settings.samples, static_cast<uint32_t>(save->history[i + 2]));
            gather = gather_constants(settings, sun, fit);
            // The stand-ins for predict_shot.py, as trace-light.bin would hold them.
            SceneLight standin;
            standin.sun = sun;
            standin.ambient = fit;
            standin.sky = map;
            std::vector<TraceTriangle> worded = scene->triangles;
            for (size_t i = 0; i < worded.size(); i++)
                worded[i].light = words[i];
            const std::string bytes = light_bytes(worded, std::vector<uint32_t>(worded.size(), 0), standin, {});
            std::ofstream(folder / "reference-light.bin", std::ios::binary | std::ios::trunc).write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
            std::printf("STANDIN the ambient fit of the save's own irradiance (%s, r2 %.4f %.4f %.4f), hit_ambient 0.5, the sun's color census 111824's "
                        "along the pushed camera's sun (%.4f, %.4f, %.4f)\n", fit.kind == AmbientFit::Kind::fit ? "fit" : "mean", fit.r2[0], fit.r2[1], fit.r2[2],
                sun.shade[0], sun.shade[1], sun.shade[2]);
            lit_copy = bvh;
            for (size_t i = 0; i < lit_copy.triangles.size(); i++)
                lit_copy.triangles[i].light = words[lit_copy.source[i]];
        }
    }
    const Bvh &lit = lit_copy.triangles.empty() ? bvh : lit_copy;
    std::vector<float> reference(size_t(width) * height * 4, 0.0f);
    // Each pixel's occlusion and sun samples squared and summed, so check_trace.py knows each pixel's sampling variance.
    std::vector<float> squares(size_t(width) * height * 2, 0.0f);
    // The gather's sums, and for each pixel its hits' and skies' luminances squared and summed, and the luminance of the
    // hits the sun did not light summed: check_trace.py splits the Deck's sum between its sun-lit hits and the others by it.
    std::vector<float> gather_hit(gathering ? size_t(width) * height * 4 : 0), gather_sky(gather_hit.size()), gather_squares(gathering ? size_t(width) * height * 3 : 0);
    std::atomic<uint64_t> gather_rays { 0 }, gather_guard { 0 }, gather_overflow { 0 };
    const std::vector<uint32_t> *alpha = masks.words.empty() ? nullptr : &masks.words;
    if (!alpha)
        std::printf("INFO the scene holds no alpha tests\n");
    std::atomic<uint32_t> next_row { 0 };
    std::atomic<uint64_t> rays { 0 }, guard { 0 }, overflow { 0 };
    std::mutex sources_lock;
    start = std::chrono::steady_clock::now();
    std::vector<std::thread> threads;
    for (uint32_t t = 0; t < options.tasks; t++)
        threads.emplace_back([&] {
            uint64_t my_rays = 0, my_guard = 0, my_overflow = 0, my_gather_rays = 0, my_gather_guard = 0, my_gather_overflow = 0;
            std::array<uint64_t, 4> my_sources {};
            for (uint32_t y; (y = next_row++) < height;)
                for (uint32_t x = 0; x < width; x++)
                {
                    const size_t at = (size_t(y) * width + x) * 4;
                    // The Deck's counts become this pixel's caps, so it gets exactly their samples; no consistency ray. While it
                    // gathers, the gather takes the occlusion samples and the trace kernel the sun's alone.
                    TraceConstants constants = save->constants;
                    constants.trace_counts[0] = gathering ? 0 : static_cast<uint32_t>(save->history[at + 2]);
                    constants.trace_counts[1] = static_cast<uint32_t>(save->history[at + 3]);
                    constants.trace_render[3] = 0;
                    if (gathering)
                    {
                        GatherConstants mine = gather;
                        mine.counts[0] = static_cast<uint32_t>(save->history[at + 2]);
                        std::array<float, 4> h {}, hit {}, sky_sum {};
                        double occlusion2 = 0, hit2 = 0, sky2 = 0, unlit = 0;
                        for (uint32_t k = mine.counts[0]; k > 0; k--)
                        {
                            const GatherPixel g = gather_pixel(lit, image, constants, mine, sky, x, y, h, hit, sky_sum, alpha);
                            if (!g.world)
                                break;
                            const double l = 0.2126 * g.light[0] + 0.7152 * g.light[1] + 0.0722 * g.light[2];
                            occlusion2 += double(g.occlusion) * g.occlusion;
                            (g.light[3] < 1.5f ? hit2 : sky2) += l * l;
                            if (g.light[3] < 1.5f && !g.lit)
                                unlit += l;
                            h = g.history;
                            hit = g.hit;
                            sky_sum = g.sky;
                            my_gather_rays += g.counters[static_cast<size_t>(GatherCounter::rays)];
                            my_gather_guard += g.counters[static_cast<size_t>(GatherCounter::guard)];
                            my_gather_overflow += g.counters[static_cast<size_t>(GatherCounter::overflow)];
                            for (size_t s = 0; s < 4; s++)
                                my_sources[s] += g.counters[static_cast<size_t>(GatherCounter::direct) + s];
                        }
                        reference[at] = h[0];
                        reference[at + 2] = h[2];
                        squares[at / 2] = static_cast<float>(occlusion2);
                        std::copy(hit.begin(), hit.end(), gather_hit.begin() + static_cast<std::ptrdiff_t>(at));
                        std::copy(sky_sum.begin(), sky_sum.end(), gather_sky.begin() + static_cast<std::ptrdiff_t>(at));
                        gather_squares[at / 4 * 3] = static_cast<float>(hit2);
                        gather_squares[at / 4 * 3 + 1] = static_cast<float>(sky2);
                        gather_squares[at / 4 * 3 + 2] = static_cast<float>(unlit);
                    }
                    std::array<float, 4> h = { reference[at], 0, reference[at + 2], 0 };
                    double square[2] = { squares[at / 2], 0 };
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
            gather_rays += my_gather_rays;
            gather_guard += my_gather_guard;
            gather_overflow += my_gather_overflow;
            std::lock_guard<std::mutex> lock(sources_lock);
            for (size_t s = 0; s < 4; s++)
                hits_by_source[s] += my_sources[s];
        });
    for (std::thread &t : threads)
        t.join();
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    std::printf("INFO traced %llu rays in %.1f s on %u threads (%.2f Mrays/s); %llu cut short by a cap, %llu stack overflows\n",
        static_cast<unsigned long long>(rays.load()), seconds, options.tasks, rays.load() / std::max(seconds, 1e-9) / 1e6,
        static_cast<unsigned long long>(guard.load()), static_cast<unsigned long long>(overflow.load()));
    // A ray its caps cut short stops wherever its work ran out: a scene that does that to more than a stray ray holds
    // something no scene should, as build 207's meadow did.
    std::snprintf(line, sizeof line, "at most 0.1%% of the rays were cut short by a cap (%.3f%%)", 100.0 * guard.load() / std::max<uint64_t>(rays.load(), 1));
    check(line, guard.load() * 1000 <= rays.load());
    if (gathering)
    {
        const uint64_t hits = hits_by_source[0] + hits_by_source[1] + hits_by_source[2] + hits_by_source[3];
        std::printf("INFO the gather traced %llu rays, %llu cut short by a cap and %llu stack overflows; its %llu hits by albedo source: direct %.1f%%, "
                    "covered %.1f%%, material %.1f%%, class %.1f%%\n", static_cast<unsigned long long>(gather_rays.load()),
            static_cast<unsigned long long>(gather_guard.load()), static_cast<unsigned long long>(gather_overflow.load()), static_cast<unsigned long long>(hits),
            100.0 * hits_by_source[0] / std::max<uint64_t>(hits, 1), 100.0 * hits_by_source[1] / std::max<uint64_t>(hits, 1),
            100.0 * hits_by_source[2] / std::max<uint64_t>(hits, 1), 100.0 * hits_by_source[3] / std::max<uint64_t>(hits, 1));
        check("no gather ray met a cap", gather_guard.load() == 0);
        bool written = true;
        for (const auto &[name, data] : { std::pair { "reference-gather-hit.f32", &gather_hit }, std::pair { "reference-gather-sky.f32", &gather_sky },
                 std::pair { "reference-gather-squares.f32", &gather_squares } })
        {
            std::ofstream file_out(folder / name, std::ios::binary | std::ios::trunc);
            file_out.write(reinterpret_cast<const char *>(data->data()), static_cast<std::streamsize>(data->size() * sizeof(float)));
            written &= static_cast<bool>(file_out);
        }
        check("reference-gather-hit.f32, reference-gather-sky.f32 and reference-gather-squares.f32 are written", written);
    }
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
    std::atomic<uint64_t> agree[2][3] {}, pixels[3] {}, cut_short { 0 };
    next_row = 0;
    threads.clear();
    for (uint32_t t = 0; t < options.tasks; t++)
        threads.emplace_back([&] {
            uint64_t my_agree[2][3] {}, my_pixels[3] {}, my_cut_short = 0;
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
                        TraversalStats stats;
                        const std::optional<Hit> hit = trace_nearest(bvh, eye, direction, distance * 1.5f + 1, tested == 0 ? kAllClasses | kAlphaTests : kAllClasses,
                            &stats, alpha);
                        my_agree[tested][group] += hit && std::fabs(hit->t - distance) <= 0.02f * distance;
                        my_cut_short += tested == 0 && stats.guard;
                    }
                }
            cut_short += my_cut_short;
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
    const uint64_t view_rays = pixels[0] + pixels[1] + pixels[2];
    std::snprintf(line, sizeof line, "at most 0.1%% of the view's rays with the alpha tests were cut short by a cap (%llu of %llu)",
        static_cast<unsigned long long>(cut_short.load()), static_cast<unsigned long long>(view_rays));
    check(line, cut_short.load() * 1000 <= view_rays);
    // Leaf cards are not the game's leaves, which the depth buffer shows.
    if (alpha && leaf_cards.empty())
        check("the alpha tests do not lower the view's depth agreement", share(0, { 0, 1, 2 }).second >= share(1, { 0, 1, 2 }).second);
    std::printf("%s: %d failed\n", __FILE__, failures);
    return failures == 0 ? 0 : 1;
}
