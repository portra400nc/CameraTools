// Bakes an ENHANCEMENTS_SCENE_LIGHT | GBUFFER capture's light again on the Mac from its faces' raw copies, as the add-on's
// writer thread did (core/light.h's build_light), and checks it against the capture's trace-light.bin to the bit: every
// triangle's light word, the sky map and its harmonics, and the ambient fit. With a trace folder of a save at the
// capture's spot, it also casts a ray from the eye through every view pixel and compares the albedo of the triangle it
// meets with the view's G-buffer albedo (albedo.rgba8), by stencil class.
//
//   bake_light <scene folder> [trace folder]
//
// Prints PASS, FAIL and INFO lines like the tests. tests/check_light.sh builds and runs it before check_light.py.
#include "../core/trace_files.h"
#include "scene_light.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>

using namespace enhancements;
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
        std::fprintf(stderr, "usage: bake_light <scene folder> [trace folder]\n");
        return 2;
    }
    const fs::path folder = argv[1];
    const std::optional<TraceSceneFile> scene = parse_trace_scene(scene_light::slurp(folder / "trace-scene.bin"));
    const std::optional<LightFile> file = parse_light(scene_light::slurp(folder / "trace-light.bin"));
    if (!scene || !file || file->words.size() != scene->triangles.size())
    {
        std::printf("FAIL %s holds no trace-scene.bin, or no trace-light.bin of its triangles\n", folder.u8string().c_str());
        return 1;
    }
    const scene_light::SceneFaces faces = scene_light::read_faces(folder, file, false);
    check("every face of the capture has its raw albedo, stencil and HDR copies, and trace-light.bin its exact camera", faces.raw && faces.exact_cameras
        && faces.faces.size() == file->faces.size());
    if (!faces.raw)
        return 1;
    LightInputs inputs;
    inputs.faces = faces.faces;
    inputs.first_size = faces.view_size;
    inputs.first_normals = faces.view_normals;
    inputs.first_irradiance = faces.view_irradiance;
    inputs.first_stencil = faces.view_stencil;
    inputs.first_depth = faces.view_depth;
    // The sun came from the combine constants, which the capture does not save; the file's stands for itself.
    inputs.pushed_sky = file->light.sky.fallback && !file->light.sky.texels.empty()
        ? Vec3f { file->light.sky.texels[0], file->light.sky.texels[1], file->light.sky.texels[2] } : Vec3f { 0, 0, 0 };
    if (file->light.ambient.kind == AmbientFit::Kind::pushed)
        inputs.pushed_sky = file->light.ambient.rows[0];
    std::vector<TraceTriangle> triangles = scene->triangles;
    const auto start = std::chrono::steady_clock::now();
    const SceneLight light = build_light(triangles, file->materials, inputs, scene->reference, std::max(1u, std::thread::hardware_concurrency()));
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    uint64_t differ = 0;
    for (size_t i = 0; i < triangles.size(); i++)
        differ += triangles[i].light != file->words[i];
    const AlbedoCounts &c = light.albedo;
    std::printf("INFO %zu triangles baked in %.2f s from %zu faces: %llu direct, %llu covered, %llu by material, %llu by class; by area %.1f%%, %.1f%%, "
                "%.1f%% and %.1f%%\n", triangles.size(), seconds, faces.faces.size(), static_cast<unsigned long long>(c.triangles[0]),
        static_cast<unsigned long long>(c.triangles[1]), static_cast<unsigned long long>(c.triangles[2]), static_cast<unsigned long long>(c.triangles[3]),
        100 * c.area[0] / (c.area[0] + c.area[1] + c.area[2] + c.area[3]), 100 * c.area[1] / (c.area[0] + c.area[1] + c.area[2] + c.area[3]),
        100 * c.area[2] / (c.area[0] + c.area[1] + c.area[2] + c.area[3]), 100 * c.area[3] / (c.area[0] + c.area[1] + c.area[2] + c.area[3]));
    check("1 every triangle's light word is the capture's (" + std::to_string(differ) + " differ)", differ == 0);
    check("1 the sky map's texels and harmonics are the capture's, " + std::to_string(light.sky.real) + " real",
        light.sky.texels == file->light.sky.texels && light.sky.harmonics == file->light.sky.harmonics && light.sky.real == file->light.sky.real);
    check("1 the ambient fit is the capture's", light.ambient.rows == file->light.ambient.rows && light.ambient.kind == file->light.ambient.kind);

    if (argc > 2)
    {
        std::string why;
        const std::optional<TraceSave> save = read_trace_folder(argv[2], why);
        if (!save || save->albedo.empty())
        {
            std::printf("FAIL %s: %s\n", argv[2], save ? "no albedo.rgba8" : why.c_str());
            return 1;
        }
        Bvh bvh = build_bvh(triangles);
        const TraceConstants &k = save->constants;
        const Vec3f eye = { k.eye_position[0], k.eye_position[1], k.eye_position[2] };
        // The mean absolute difference of the met triangle's albedo and the pixel's, by stencil: 128, 144, 136, 137.
        const uint8_t classes[] = { 128, 144, 136, 137 };
        double sums[4] = {};
        uint64_t counts[4] = {};
        for (uint32_t y = 0; y < save->size.height; y++)
            for (uint32_t x = 0; x < save->size.width; x++)
            {
                const size_t at = size_t(y) * save->size.width + x;
                const auto group = std::find(std::begin(classes), std::end(classes), save->stencil[at]) - std::begin(classes);
                if (group == 4 || !(save->depth[at] > 0))
                    continue;
                float u = (x + 0.5f) / save->size.width, v = (y + 0.5f) / save->size.height;
                if (k.trace_render[2] != 0)
                    v = 1 - v;
                const float clip[4] = { u * 2 - 1, 1 - v * 2, save->depth[at], 1 };
                float h[4] = {};
                for (int r = 0; r < 4; r++)
                    for (int i = 0; i < 4; i++)
                        h[r] += k.clip_rows[r][i] * clip[i];
                const Vec3f d = { h[0] / h[3] - eye[0], h[1] / h[3] - eye[1], h[2] / h[3] - eye[2] };
                const float distance = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
                const std::optional<Hit> hit = trace_nearest(bvh, eye, { d[0] / distance, d[1] / distance, d[2] / distance }, distance * 1.5f + 1);
                if (!hit || std::fabs(hit->t - distance) > 0.02f * distance)
                    continue;
                const Vec3f albedo = light_albedo(bvh.triangles[hit->triangle].light);
                double difference = 0;
                for (int i = 0; i < 3; i++)
                    difference += std::fabs(albedo[i] - srgb_linear(save->albedo[at * 4 + i])) / 3;
                sums[group] += difference;
                counts[group]++;
            }
        const auto mad = [&](int g) { return counts[g] ? sums[g] / counts[g] : 0.0; };
        std::printf("INFO the triangle a view ray meets against the view's G-buffer albedo, mean absolute difference: stencil 128 %.4f of %llu, 144 %.4f "
                    "of %llu, 136 %.4f of %llu, 137 %.4f of %llu\n", mad(0), static_cast<unsigned long long>(counts[0]), mad(1),
            static_cast<unsigned long long>(counts[1]), mad(2), static_cast<unsigned long long>(counts[2]), mad(3), static_cast<unsigned long long>(counts[3]));
        const double world = counts[0] + counts[1] ? (sums[0] + sums[1]) / double(counts[0] + counts[1]) : 0;
        check("2 the albedo of the triangles the view's rays meet is the G-buffer's within 0.05 on stencil 128 and 144", counts[0] + counts[1] > 0 && world <= 0.05);
    }
    std::printf("%s: %d failed\n", __FILE__, failures);
    return failures == 0 ? 0 : 1;
}
