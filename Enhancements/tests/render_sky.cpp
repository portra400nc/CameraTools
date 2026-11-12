// Draws sky maps as fisheye pictures of the upper half, the zenith in the middle and the horizon at the rim, the light's
// side to the right, through the game's tone curve:
//
//   render_sky <out.png> [scene folder]
//
// The first row is the physical sky at 60, 20, 5, 1, -3 and -8 degrees of the light's elevation, each tile exposed so its
// mean is middle gray. With a scene folder (trace-light.bin), a second row puts the scene's own sky map, the one the
// game's six faces gave, beside the physical sky for the same sun and light, both at one exposure, so their brightness
// compares.
#include "../core/png.h"
#include "../core/sky.h"
#include "scene_light.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>

using namespace enhancements;

namespace
{
    constexpr uint32_t kTile = 256;

    float luminance(const Vec3f &c) { return 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2]; }

    // shaders/gather.hlsli's sky_texel_of: the octahedral map's texel of a direction.
    Vec3f texel_of(const SkyMap &sky, const Vec3f &d)
    {
        const float sum = std::fabs(d[0]) + std::fabs(d[1]) + std::fabs(d[2]);
        float x = d[0] / sum, z = d[2] / sum;
        if (d[1] < 0)
        {
            const float fx = (1 - std::fabs(z)) * (x >= 0 ? 1.0f : -1.0f), fz = (1 - std::fabs(x)) * (z >= 0 ? 1.0f : -1.0f);
            x = fx;
            z = fz;
        }
        const uint32_t tx = std::min(uint32_t((x * 0.5f + 0.5f) * kSkyMapSide), kSkyMapSide - 1);
        const uint32_t tz = std::min(uint32_t((z * 0.5f + 0.5f) * kSkyMapSide), kSkyMapSide - 1);
        const float *t = &sky.texels[(size_t(tz) * kSkyMapSide + tx) * 4];
        return { t[0], t[1], t[2] };
    }

    uint8_t tone(float x)
    {
        x = std::max(x, 0.0f);
        const float y = std::min(x * (1.36f * x + 0.047f) / (x * (0.93f * x + 0.56f) + 0.14f), 1.0f);
        const float s = y <= 0.0031308f ? 12.92f * y : 1.055f * std::pow(y, 1 / 2.4f) - 0.055f;
        return uint8_t(std::lround(std::min(std::max(s, 0.0f), 1.0f) * 255));
    }

    // The fisheye's direction at a tile pixel, the light's azimuth (toward_light's x and z) to the right; empty past the rim.
    bool direction_at(uint32_t px, uint32_t py, const Vec3f &toward_light, Vec3f &d)
    {
        const float x = (px + 0.5f) / kTile * 2 - 1, y = 1 - (py + 0.5f) / kTile * 2, r = std::sqrt(x * x + y * y);
        if (r > 1)
            return false;
        const float elevation = (1 - r) * 1.5707963f, azimuth = std::atan2(y, x);
        const float flat = std::sqrt(toward_light[0] * toward_light[0] + toward_light[2] * toward_light[2]);
        const float ax = flat > 1e-6f ? toward_light[0] / flat : 1, az = flat > 1e-6f ? toward_light[2] / flat : 0;
        const float c = std::cos(azimuth), s = std::sin(azimuth);
        const float dx = c * ax - s * az, dz = c * az + s * ax;
        d = { std::cos(elevation) * dx, std::sin(elevation), std::cos(elevation) * dz };
        return true;
    }

    float mean_luminance(const SkyMap &sky, const Vec3f &toward_light)
    {
        double sum = 0;
        uint32_t n = 0;
        for (uint32_t py = 0; py < kTile; py += 4)
            for (uint32_t px = 0; px < kTile; px += 4)
            {
                Vec3f d;
                if (direction_at(px, py, toward_light, d))
                {
                    sum += luminance(texel_of(sky, d));
                    n++;
                }
            }
        return n ? float(sum / n) : 0;
    }

    void draw(std::vector<uint8_t> &rgb, uint32_t width, uint32_t column, uint32_t row, const SkyMap &sky, const Vec3f &toward_light, float exposure)
    {
        for (uint32_t py = 0; py < kTile; py++)
            for (uint32_t px = 0; px < kTile; px++)
            {
                Vec3f d;
                if (!direction_at(px, py, toward_light, d))
                    continue;
                const Vec3f c = texel_of(sky, d);
                uint8_t *out = &rgb[((size_t(row) * kTile + py) * width + column * kTile + px) * 3];
                for (int k = 0; k < 3; k++)
                    out[k] = tone(c[k] * exposure);
            }
    }

    Vec3f toward(float degrees, const Vec3f &azimuth_of)
    {
        const float flat = std::sqrt(azimuth_of[0] * azimuth_of[0] + azimuth_of[2] * azimuth_of[2]);
        const float ax = flat > 1e-6f ? azimuth_of[0] / flat : 1, az = flat > 1e-6f ? azimuth_of[2] / flat : 0;
        const float e = degrees * 3.14159265f / 180;
        return { std::cos(e) * ax, std::sin(e), std::cos(e) * az };
    }
}

int main(int argc, char **argv)
{
    if (argc < 2)
    {
        std::fprintf(stderr, "render_sky <out.png> [scene folder]\n");
        return 2;
    }
    const Atmosphere air = make_atmosphere(SkySettings {});
    std::optional<LightFile> scene;
    if (argc > 2)
        scene = parse_light(scene_light::slurp(std::filesystem::path(argv[2]) / "trace-light.bin"));
    const float elevations[] = { 60, 20, 5, 1, -3, -8 };
    const uint32_t columns = 6, rows = scene ? 2 : 1, width = columns * kTile;
    std::vector<uint8_t> rgb(size_t(width) * rows * kTile * 3, 0);
    const Vec3f shade = scene ? Vec3f { float(scene->light.sun.color[0]), float(scene->light.sun.color[1]), float(scene->light.sun.color[2]) } : Vec3f { 1, 1, 1 };
    const Vec3f azimuth = scene ? Vec3f { float(scene->light.sun.shade[0]), 0, float(scene->light.sun.shade[2]) } : Vec3f { 1, 0, 0 };
    for (uint32_t i = 0; i < columns; i++)
    {
        const Vec3f light = toward(elevations[i], azimuth);
        const SkyMap sky = physical_sky_map(air, light, shade);
        const float mean = mean_luminance(sky, light);
        std::printf("light at %g degrees: mean sky luminance %.5f\n", elevations[i], mean);
        draw(rgb, width, i, 0, sky, light, mean > 0 ? 0.18f / mean : 1);
    }
    if (scene)
    {
        const Vec3f light = { float(scene->light.sun.shade[0]), float(scene->light.sun.shade[1]), float(scene->light.sun.shade[2]) };
        const SkyMap physical = physical_sky_map(air, light, shade);
        const float game_mean = mean_luminance(scene->light.sky, light), physical_mean = mean_luminance(physical, light);
        std::printf("scene: sun at %.1f degrees, its light shows %.3f; mean sky luminance: the game's %.5f, the physical %.5f\n",
            std::asin(light[1]) * 180 / 3.14159265f, luminance(shade), game_mean, physical_mean);
        const float exposure = 0.18f / std::max(game_mean, 1e-6f);
        draw(rgb, width, 0, 1, scene->light.sky, light, exposure);
        draw(rgb, width, 1, 1, physical, light, exposure);
        draw(rgb, width, 2, 1, physical, light, 0.18f / std::max(physical_mean, 1e-6f));
    }
    std::ofstream(argv[1], std::ios::binary) << encode_png(rgb.data(), width, rows * kTile);
    return 0;
}
