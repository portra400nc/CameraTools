// The depth of field on the Mac: renders a frame EnhancementsSaveFrame saved through core/dof.cpp's passes, which run
// shaders/dof.hlsli's own code, and writes it as a PNG through the game's tone curve, without bloom, so the look can be
// tuned against a reference picture before a Deck round.
//
//     render_dof <frame folder> <out.png> [key=value ...]
//     render_dof --synthetic <frame folder>
//
// Keys, the depth of field's settings (enabled; the rest at their defaults): f (f-number), sensor (mm), focus (meters, 0
// autofocus), x and y (the focus point, 0 to 1 from the top left), speed, max_blur, blades, roundness, rotation (degrees),
// sa (spherical aberration), cat_eye, swirl, anamorphic, chromatic, samples, threshold and gain (the highlights'), and
// scatter (0 or 1);
// and frames (frames averaged, each with the frame's own turn of the spiral, as the accumulator averages a paused shot),
// exposure (0.985, the census's), and crop (x,y,w,h in the picture's pixels from the top left) with zoom (a whole number)
// to write a magnified part. Exits with 3 when a half-float target held infinity or NaN once stored as R16F.
//
// --synthetic writes a 1280x800 frame folder: a 50 mm lens's view of a sharp, striped subject 2 m away, a tree trunk at 6 m,
// a leafy branch 1 m away at the bottom right, and foliage 30 m away full of small bright sky gaps and thin lit and dark
// branches, the kind of background that shows soap-bubble bokeh.
#include "../core/dof.h"
#include "../core/light.h"
#include "../core/png.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <random>
#include <string>
#include <vector>

using namespace enhancements;

namespace
{
    float tone(float x, float exposure)
    {
        x = std::max(x * exposure, 0.0f);
        const float curve = std::clamp(x * (1.36f * x + 0.047f) / (x * (0.93f * x + 0.56f) + 0.14f), 0.0f, 1.0f);
        return std::clamp(std::max(std::pow(curve, 1 / 2.4f) * 1.055f - 0.055f, 0.0f), 0.0f, 1.0f);
    }

    bool set(DepthOfFieldSettings &s, const std::string &key, float v)
    {
        if (key == "f") s.f_number = v;
        else if (key == "sensor") s.sensor_height_mm = v;
        else if (key == "focus") s.focus_distance = v;
        else if (key == "x") s.focus_point[0] = v;
        else if (key == "y") s.focus_point[1] = v;
        else if (key == "speed") s.focus_speed = v;
        else if (key == "max_blur") s.max_blur = v;
        else if (key == "blades") s.blades = static_cast<uint32_t>(v);
        else if (key == "roundness") s.blade_roundness = v;
        else if (key == "rotation") s.blade_rotation = v;
        else if (key == "sa") s.spherical_aberration = v;
        else if (key == "cat_eye") s.cat_eye = v;
        else if (key == "swirl") s.swirl = v;
        else if (key == "anamorphic") s.anamorphic = v;
        else if (key == "chromatic") s.chromatic = v;
        else if (key == "samples") s.samples = static_cast<uint32_t>(v);
        else if (key == "threshold") s.highlight_threshold = v;
        else if (key == "gain") s.highlight_gain = v;
        else if (key == "scatter") s.scatter = v != 0;
        else return false;
        return true;
    }

    // A 50 mm lens on a 24 mm sensor (P11 = 50 / 12) at 16:10, near 0.1 m and far 1000 m, reversed Z, column-major.
    Camera synthetic_camera()
    {
        Camera c {};
        const float n = 0.1f, f = 1000, p11 = 50.0f / 12.0f;
        c.view_to_clip[0] = p11 / 1.6f;
        c.view_to_clip[5] = p11;
        c.view_to_clip[10] = n / (f - n);
        c.view_to_clip[11] = -1;
        c.view_to_clip[14] = n * f / (f - n);
        for (int i = 0; i < 4; i++)
            c.world_to_view[i * 5] = 1;
        return c;
    }

    // Smooth value noise in 0..1 over a lattice of cell pixels.
    struct Noise
    {
        std::vector<float> lattice;
        int side = 64;
        explicit Noise(uint32_t seed) : lattice(64 * 64)
        {
            std::mt19937 random(seed);
            for (float &v : lattice)
                v = std::uniform_real_distribution<float>(0, 1)(random);
        }
        float operator()(float x, float y) const
        {
            const int x0 = static_cast<int>(std::floor(x)), y0 = static_cast<int>(std::floor(y));
            const float fx = x - x0, fy = y - y0, sx = fx * fx * (3 - 2 * fx), sy = fy * fy * (3 - 2 * fy);
            const auto at = [this](int i, int j) { return lattice[size_t(((j % side) + side) % side) * side + ((i % side) + side) % side]; };
            return (at(x0, y0) * (1 - sx) + at(x0 + 1, y0) * sx) * (1 - sy) + (at(x0, y0 + 1) * (1 - sx) + at(x0 + 1, y0 + 1) * sx) * sy;
        }
    };

    int synthetic(const std::string &folder)
    {
        const uint32_t w = 1280, h = 800;
        FrameSave frame;
        frame.size = { w, h };
        frame.flip = true;
        frame.camera = synthetic_camera();
        std::vector<Vec3f> color(size_t(w) * h);
        std::vector<float> meters(size_t(w) * h, 30);
        const Noise leaves(1), clumps(2), bark(3);
        // The background: dark foliage in clumps, lit unevenly.
        for (uint32_t y = 0; y < h; y++)
            for (uint32_t x = 0; x < w; x++)
            {
                const float n = 0.6f * leaves(x / 6.0f, y / 6.0f) + 0.4f * clumps(x / 40.0f, y / 40.0f);
                color[size_t(y) * w + x] = { 0.015f + 0.05f * n, 0.03f + 0.09f * n, 0.012f + 0.03f * n };
            }
        std::mt19937 random(7);
        const auto uniform = [&random](float a, float b) { return std::uniform_real_distribution<float>(a, b)(random); };
        // Thin branches, lit and dark, as polylines that wander.
        for (int i = 0; i < 70; i++)
        {
            const bool lit = i % 3 != 0;
            float x = uniform(0, w), y = uniform(0, h), angle = uniform(0, 6.2832f);
            const float width = uniform(0.6f, 1.8f);
            const Vec3f c = lit ? Vec3f { 0.22f, 0.23f, 0.2f } : Vec3f { 0.006f, 0.005f, 0.004f };
            for (int step = 0; step < 300; step++)
            {
                angle += uniform(-0.08f, 0.08f);
                x += std::cos(angle);
                y += std::sin(angle);
                for (int dy = -2; dy <= 2; dy++)
                    for (int dx = -2; dx <= 2; dx++)
                    {
                        const int px = static_cast<int>(x) + dx, py = static_cast<int>(y) + dy;
                        if (px < 0 || py < 0 || px >= int(w) || py >= int(h) || std::hypot(px + 0.5f - x, py + 0.5f - y) > width)
                            continue;
                        color[size_t(py) * w + px] = c;
                    }
            }
        }
        // Sky gaps: small bright spots, bluish white, mostly where the foliage clumps are thin.
        for (int i = 0; i < 1400; i++)
        {
            const float x = uniform(0, w), y = uniform(0, h * 0.8f);
            if (clumps(x / 40.0f, y / 40.0f) > 0.55f)
                continue;
            const float radius = uniform(0.4f, 2.2f), bright = uniform(3, 9);
            for (int dy = -3; dy <= 3; dy++)
                for (int dx = -3; dx <= 3; dx++)
                {
                    const int px = static_cast<int>(x) + dx, py = static_cast<int>(y) + dy;
                    if (px >= 0 && py >= 0 && px < int(w) && py < int(h) && std::hypot(px + 0.5f - x, py + 0.5f - y) <= radius)
                        color[size_t(py) * w + px] = { 0.8f * bright, 0.9f * bright, bright };
                }
        }
        // A trunk at 6 m.
        for (uint32_t y = 0; y < h; y++)
            for (uint32_t x = 900; x < 950; x++)
            {
                const float b = 0.6f + 0.4f * bark(x / 3.0f, y / 12.0f);
                color[size_t(y) * w + x] = { 0.06f * b, 0.045f * b, 0.03f * b };
                meters[size_t(y) * w + x] = 6;
            }
        // The subject at 2 m: an upright ellipse with fine stripes and a lit rim.
        for (uint32_t y = 0; y < h; y++)
            for (uint32_t x = 0; x < w; x++)
            {
                const float u = (x - 360.0f) / 170.0f, v = (y - 430.0f) / 260.0f, r = u * u + v * v;
                if (r > 1)
                    continue;
                const float stripe = ((x / 6 + y / 6) % 2 == 0) ? 1.0f : 0.75f;
                const float light = 0.5f + 0.5f * std::max(0.0f, -u) + (r > 0.92f && u > 0.3f ? 1.5f : 0.0f);
                color[size_t(y) * w + x] = { 0.35f * stripe * light, 0.22f * stripe * light, 0.16f * stripe * light };
                meters[size_t(y) * w + x] = 2.0f + 0.15f * r;
            }
        // A leafy branch 1 m away at the bottom right, with a few glints.
        for (uint32_t y = 600; y < h; y++)
            for (uint32_t x = 1000; x < w; x++)
            {
                const float t = (x - 1000.0f) / 280.0f, edge = 700 - 60 * std::sin(t * 5);
                if (y < edge)
                    continue;
                const bool glint = (x % 37 == 0 && y % 29 == 0);
                color[size_t(y) * w + x] = glint ? Vec3f { 4, 4, 3 } : Vec3f { 0.05f, 0.12f, 0.03f };
                meters[size_t(y) * w + x] = 1;
            }
        const std::optional<std::array<float, 4>> k = dof_depth_coefficients(frame.camera);
        frame.hdr.resize(color.size());
        frame.depth.resize(color.size());
        // Stored upside down: stored row 0 is the picture's bottom row.
        for (uint32_t y = 0; y < h; y++)
            for (uint32_t x = 0; x < w; x++)
            {
                const size_t from = size_t(y) * w + x, to = size_t(h - 1 - y) * w + x;
                frame.hdr[to] = hdr_stored(color[from]);
                // 1 / z = -(k0 d + k1) / (k2 d + k3), solved for d.
                const float inverse = 1 / meters[from];
                frame.depth[to] = -((*k)[1] + inverse * (*k)[3]) / ((*k)[0] + inverse * (*k)[2]);
            }
        const std::vector<std::string> failed = write_frame_folder(folder, frame);
        for (const std::string &name : failed)
            std::fprintf(stderr, "could not write %s\n", name.c_str());
        std::printf("wrote %s: 1280x800, subject 2 m, trunk 6 m, branch 1 m, foliage 30 m\n", folder.c_str());
        return failed.empty() ? 0 : 1;
    }
}

int main(int argc, char **argv)
{
    if (argc == 3 && std::string(argv[1]) == "--synthetic")
        return synthetic(argv[2]);
    if (argc < 3)
    {
        std::fprintf(stderr, "usage: render_dof <frame folder> <out.png> [key=value ...]\n       render_dof --synthetic <frame folder>\n");
        return 2;
    }
    std::string why;
    const std::optional<FrameSave> frame = read_frame_folder(argv[1], why);
    if (!frame)
    {
        std::fprintf(stderr, "%s: %s\n", argv[1], why.c_str());
        return 1;
    }
    DepthOfFieldSettings settings;
    settings.enabled = true;
    uint32_t frames = 1;
    float exposure = 0.985f;
    int crop[4] = { 0, 0, int(frame->size.width), int(frame->size.height) };
    int zoom = 1;
    for (int i = 3; i < argc; i++)
    {
        const std::string arg = argv[i];
        const size_t eq = arg.find('=');
        const std::string key = arg.substr(0, eq), value = eq == std::string::npos ? "" : arg.substr(eq + 1);
        if (key == "frames")
            frames = std::max(1, std::atoi(value.c_str()));
        else if (key == "exposure")
            exposure = std::strtof(value.c_str(), nullptr);
        else if (key == "zoom")
            zoom = std::max(1, std::atoi(value.c_str()));
        else if (key == "crop" && std::sscanf(value.c_str(), "%d,%d,%d,%d", &crop[0], &crop[1], &crop[2], &crop[3]) == 4)
            continue;
        else if (eq == std::string::npos || !set(settings, key, std::strtof(value.c_str(), nullptr)))
        {
            std::fprintf(stderr, "unknown argument %s\n", arg.c_str());
            return 2;
        }
    }
    settings = clamp_depth_of_field(settings);
    DofImage image;
    image.size = frame->size;
    image.depth = frame->depth;
    image.rgb.resize(frame->hdr.size() * 3);
    for (size_t i = 0; i < frame->hdr.size(); i++)
    {
        const Vec3f c = hdr_rgb(frame->hdr[i]);
        std::copy(c.begin(), c.end(), &image.rgb[i * 3]);
    }
    const auto start = std::chrono::steady_clock::now();
    std::vector<float> sum(image.rgb.size(), 0.0f);
    float focus = 0, luminance = 0;
    uint32_t sprites[2] = {}, nonfinite = 0;
    for (uint32_t f = 0; f < frames; f++)
    {
        const std::optional<DofConstants> constants = dof_constants(settings, frame->camera, frame->size, frame->flip, 0, f);
        if (!constants)
        {
            std::fprintf(stderr, "the frame's projection does not invert\n");
            return 1;
        }
        const DofOutput out = render_dof(image, *constants, focus);
        focus = out.focus;
        luminance = out.luminance;
        sprites[0] = out.sprites[0];
        sprites[1] = out.sprites[1];
        nonfinite += out.nonfinite;
        for (size_t i = 0; i < sum.size(); i++)
            sum[i] += out.rgb[i] / frames;
    }
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    const uint32_t w = frame->size.width, h = frame->size.height;
    crop[0] = std::clamp(crop[0], 0, int(w) - 1);
    crop[1] = std::clamp(crop[1], 0, int(h) - 1);
    crop[2] = std::clamp(crop[2], 1, int(w) - crop[0]);
    crop[3] = std::clamp(crop[3], 1, int(h) - crop[1]);
    const uint32_t ow = uint32_t(crop[2] * zoom), oh = uint32_t(crop[3] * zoom);
    std::vector<uint8_t> rgb(size_t(ow) * oh * 3);
    for (uint32_t y = 0; y < oh; y++)
        for (uint32_t x = 0; x < ow; x++)
        {
            const uint32_t px = crop[0] + x / zoom, py = crop[1] + y / zoom;
            const size_t at = (size_t(frame->flip ? h - 1 - py : py) * w + px) * 3;
            for (int c = 0; c < 3; c++)
                rgb[(size_t(y) * ow + x) * 3 + c] = static_cast<uint8_t>(tone(sum[at + c], exposure) * 255 + 0.5f);
        }
    std::ofstream(argv[2], std::ios::binary) << encode_png(rgb.data(), ow, oh);
    double energy = 0;
    for (const float v : sum)
        energy += v;
    std::printf("%s: focus %.3f m, mean luminance %.3f, %u far and %u near sprites, energy %.6f a channel, %u non-finite, %u frame%s in %.2f s\n",
        argv[2], focus, luminance, sprites[0], sprites[1], energy / double(sum.size()), nonfinite, frames, frames == 1 ? "" : "s", seconds);
    return nonfinite == 0 ? 0 : 3;
}
