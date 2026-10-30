// The depth of field on the Mac: renders a frame EnhancementsSaveFrame saved through core/dof.cpp's passes, which run
// shaders/dof.hlsli's own code, and writes it as a PNG through the game's tone curve, without bloom, so the look can be
// tuned against a reference picture before a Deck round.
//
//     render_dof <frame folder> <out.png> [key=value ...]
//     render_dof --synthetic <frame folder>
//     render_dof --chromatic-sweep <frames folder> [key=value ...]
//     render_dof --chromatic-check <frame folder> <most flagged> [key=value ...] [mask=<out.png>]
//     render_dof --seam-sweep <frames folder> [key=value ...]
//     render_dof --seam-check <frame folder> <most levels> [key=value ...]
//     render_dof --near-edge-check <most share off> [key=value ...]
//     render_dof --far-edge-check <most share off> <most halo> [key=value ...]
//
// Keys, the depth of field's settings (enabled; the rest at their defaults): f (f-number), sensor (mm), focus (meters, 0
// autofocus), x and y (the focus point, 0 to 1 from the top left), focus_seconds, max_blur, blades, roundness, rotation (degrees),
// sa (spherical aberration), cat_eye, swirl, anamorphic, chromatic, samples, threshold and gain (the highlights'),
// scatter (0 or 1), contrast (the bokeh contrast), eye (meters to where an eye's surface under the focus point is
// reckoned, which the autofocus then finds the surface near) and eye_size (the eye's window's width in meters);
// and frames (frames averaged, each with the frame's own turn of the spiral, as the accumulator averages a paused shot),
// first (the first of them, 0 by default, so two renders compare consecutive live frames),
// display (1: the display place, as a screenshot runs it after ReShade's effects: the frame through the tone curve into an
// 8-bit sRGB back buffer, which render_dof_display decodes, blurs and encodes; 0, the default, the scene place in HDR),
// exposure (0.985, the census's), and crop (x,y,w,h in the picture's pixels from the top left) with zoom (a whole number)
// to write a magnified part, upscale (a whole number: the frame that many times its size, its color bilinear as anti-aliasing
// leaves an outline and its depth nearest, as a 4K shot's depth holds its stair-steps), and near_steps (meters) with
// most_steps (levels): the most two neighbors within the crop, both nearer than near_steps, differ by, which inside a blurred
// foreground should be small, exiting with 1 past most_steps; mm sets the sensor that gives that focal length on the frame's projection where a full-frame
// diagonal's readout (sensor 22.95) gives sensor_height / 2 P11. Exits with 3 when a half-float target held infinity or NaN
// once stored as R16F.
//
// --synthetic writes a 1280x800 frame folder: a 50 mm lens's view of a sharp, striped subject 2 m away, a tree trunk at 6 m,
// a leafy branch 1 m away at the bottom right, and foliage 30 m away full of small bright sky gaps and thin lit and dark
// branches, the kind of background that shows soap-bubble bokeh.
//
// --chromatic-sweep renders every frame folder in the folder at chromatic 0 and 1 with the given settings (by default the
// user's: f/2.8, spherical aberration 0.6, cat's eye 0.6, bokeh contrast 0.5, highlight discs from 2x at gain 2) at focal lengths
// of 90, 120, 180 and 300 mm, focused at the autofocus's distance and at 0.98, 1.02, 0.85 and 1.3 times it, and counts the
// pixels whose chromatic aberration is broken (chromatic_flags). --chromatic-check renders one frame so and exits with 1
// when more than most flagged pixels are; mask, last, writes its chromatic render with them painted magenta (uncompressed).
//
// --seam-check renders one frame at the user's settings and the given ones, with their bokeh contrast and without, and
// exits with 1 when what the contrast changes steps by more than most levels of the tone curve across a gather tile's edge
// (worst_seam); it also prints the same measure across the tiles' middles, where no seam can be, as the control.
// --seam-sweep does so for every frame folder at 50, 90, 180 and 300 mm, focused at the autofocus's distance and at 0.85
// and 1.3 times it.
//
// --near-edge-check renders a blurred plane's edge over a background in focus and in front of it (near_edge_at), and a
// farther plane's edge beside it (layer_edge_at), and exits with 1 when a 10-90% width is off a disc's edge spread by more
// than the share most.
//
// --far-edge-check renders a subject in focus with an anti-aliased outline over a background far out of focus
// (far_edge_at) and exits with 1 when a pixel along the outline, or the background past it, is off what went in.
#include "../core/dof.h"
#include "../core/light.h"
#include "../core/png.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <numeric>
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

    constexpr float kReadoutSensor = 22.95f;

    // The sensor height that gives mm on a projection of P11 p11.
    float sensor_for(float mm, float p11) { return kReadoutSensor * mm / (kReadoutSensor * 0.5f * p11); }

    bool set(DepthOfFieldSettings &s, const std::string &key, float v)
    {
        if (key == "f") s.f_number = v;
        else if (key == "sensor") s.sensor_height_mm = v;
        else if (key == "focus") s.focus_distance = v;
        else if (key == "x") s.focus_point[0] = v;
        else if (key == "y") s.focus_point[1] = v;
        else if (key == "focus_seconds") s.focus_seconds = v;
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
        else if (key == "contrast") s.bokeh_contrast = v;
        else if (key == "eye") s.eye_distance = v;
        else if (key == "eye_size") s.eye_size = v;
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

    DofImage image_of(const FrameSave &frame)
    {
        DofImage image;
        image.size = frame.size;
        image.depth = frame.depth;
        image.rgb.resize(frame.hdr.size() * 3);
        for (size_t i = 0; i < frame.hdr.size(); i++)
        {
            const Vec3f c = hdr_rgb(frame.hdr[i]);
            std::copy(c.begin(), c.end(), &image.rgb[i * 3]);
        }
        return image;
    }

    // Applies key=value arguments from first on; mm needs the frame's P11. Returns the first it does not know, or empty.
    std::string apply(DepthOfFieldSettings &s, float p11, int first, int argc, char **argv)
    {
        for (int i = first; i < argc; i++)
        {
            const std::string arg = argv[i];
            const size_t eq = arg.find('=');
            if (eq == std::string::npos)
                return arg;
            const std::string key = arg.substr(0, eq);
            const float v = std::strtof(arg.substr(eq + 1).c_str(), nullptr);
            if (key == "mm")
                s.sensor_height_mm = sensor_for(v, p11);
            else if (!set(s, key, v))
                return arg;
        }
        return {};
    }

    struct ChromaticFlags
    {
        uint32_t dropped = 0;   // a channel near 0 where the others are not
        uint32_t tiled = 0;     // a hue jump that fills most of a gather tile
        uint32_t total() const { return dropped + tiled; }
    };

    // Pixels where chromatic aberration broke the picture: compared with both the render at chromatic 0 and the sharp frame,
    // a channel's share of the pixel's light fell below 0.3 of the smaller of theirs, which is at least 0.1 (a channel
    // dropped), or every channel's share moved by more than 0.08 in a 16x16 block of the full-size picture, a gather tile,
    // where more than half the pixels moved so (a hue jump the size of a tile). Real fringes are a few pixels wide along
    // depth edges and fill no tile. Pixels darker than 0.05 a channel in any of the three are not judged.
    ChromaticFlags chromatic_flags(const std::vector<float> &sharp, const std::vector<float> &plain, const std::vector<float> &chromatic, Size size,
        std::vector<uint8_t> *flagged = nullptr)
    {
        const uint32_t w = size.width, h = size.height, tiles_x = (w + 15) / 16;
        std::vector<uint8_t> jumped(size_t(w) * h);
        std::vector<uint32_t> tile(size_t(tiles_x) * ((h + 15) / 16));
        ChromaticFlags f;
        for (size_t i = 0; i < jumped.size(); i++)
        {
            const float *a = &chromatic[i * 3], *b = &plain[i * 3], *c = &sharp[i * 3];
            const float sa = a[0] + a[1] + a[2], sb = b[0] + b[1] + b[2], sc = c[0] + c[1] + c[2];
            if (!(std::min(std::min(sa, sb), sc) > 0.15f))
                continue;
            float from_plain = 0, from_sharp = 0;
            bool dropped = false;
            for (int k = 0; k < 3; k++)
            {
                const float share = a[k] / sa, reference = std::min(b[k] / sb, c[k] / sc);
                from_plain = std::max(from_plain, std::fabs(share - b[k] / sb));
                from_sharp = std::max(from_sharp, std::fabs(share - c[k] / sc));
                dropped = dropped || (reference > 0.1f && share < 0.3f * reference);
            }
            if (dropped)
            {
                f.dropped++;
                if (flagged)
                    (*flagged)[i] = 1;
            }
            else if (std::min(from_plain, from_sharp) > 0.08f)
            {
                jumped[i] = 1;
                tile[(i / w / 16) * tiles_x + (i % w) / 16]++;
            }
        }
        for (size_t i = 0; i < jumped.size(); i++)
            if (jumped[i] && tile[(i / w / 16) * tiles_x + (i % w) / 16] > 128)
            {
                f.tiled++;
                if (flagged)
                    (*flagged)[i] = 1;
            }
        return f;
    }

    DofOutput render_at(const DofImage &image, const FrameSave &frame, const DepthOfFieldSettings &settings)
    {
        return render_dof(image, *dof_constants(clamp_depth_of_field(settings), frame.camera, frame.size, frame.flip, 0, 0), 0);
    }

    // The picture through the tone curve, top row first, with the flagged pixels, when given, painted magenta.
    void write_png(const char *path, const std::vector<float> &rgb, const FrameSave &frame, float exposure, const std::vector<uint8_t> *flagged = nullptr)
    {
        const uint32_t w = frame.size.width, h = frame.size.height;
        std::vector<uint8_t> out(size_t(w) * h * 3);
        for (uint32_t y = 0; y < h; y++)
            for (uint32_t x = 0; x < w; x++)
            {
                const size_t at = size_t(frame.flip ? h - 1 - y : y) * w + x;
                for (int c = 0; c < 3; c++)
                    out[(size_t(y) * w + x) * 3 + c] = flagged && (*flagged)[at] ? (c == 1 ? 0 : 255) : static_cast<uint8_t>(tone(rgb[at * 3 + c], exposure) * 255 + 0.5f);
            }
        std::ofstream(path, std::ios::binary) << encode_png(out.data(), w, h);
    }

    // The flags of one setting, from renders at chromatic 0 and 1; with mask, the chromatic render with them painted.
    ChromaticFlags flags_at(const DofImage &image, const FrameSave &frame, DepthOfFieldSettings settings, uint32_t &nonfinite, const char *mask = nullptr)
    {
        settings.chromatic = 0;
        const DofOutput plain = render_at(image, frame, settings);
        settings.chromatic = 1;
        const DofOutput chromatic = render_at(image, frame, settings);
        nonfinite += plain.nonfinite + chromatic.nonfinite;
        std::vector<uint8_t> flagged(image.depth.size());
        const ChromaticFlags f = chromatic_flags(image.rgb, plain.rgb, chromatic.rgb, frame.size, &flagged);
        if (mask)
            write_png(mask, chromatic.rgb, frame, 0.985f, &flagged);
        return f;
    }

    DepthOfFieldSettings user_settings()
    {
        DepthOfFieldSettings s;
        s.enabled = true;
        s.f_number = 2.8f;
        s.spherical_aberration = 0.6f;
        s.cat_eye = 0.6f;
        s.bokeh_contrast = 0.5f;
        s.highlight_threshold = 2;
        s.highlight_gain = 2;
        s.scatter = true;
        s.focus_seconds = 0;
        return s;
    }

    int chromatic_check(int argc, char **argv)
    {
        std::string why;
        const std::optional<FrameSave> frame = read_frame_folder(argv[2], why);
        if (!frame || !dof_depth_coefficients(frame->camera))
        {
            std::fprintf(stderr, "%s: %s\n", argv[2], frame ? "the projection does not invert" : why.c_str());
            return 2;
        }
        DepthOfFieldSettings settings = user_settings();
        const char *mask = nullptr;
        if (argc > 4 && std::string(argv[argc - 1]).rfind("mask=", 0) == 0)
            mask = argv[--argc] + 5;
        const std::string unknown = apply(settings, frame->camera.view_to_clip[5], 4, argc, argv);
        if (!unknown.empty())
        {
            std::fprintf(stderr, "unknown argument %s\n", unknown.c_str());
            return 2;
        }
        const uint32_t most = static_cast<uint32_t>(std::strtoul(argv[3], nullptr, 10));
        uint32_t nonfinite = 0;
        const ChromaticFlags f = flags_at(image_of(*frame), *frame, settings, nonfinite, mask);
        std::printf("%s: %u pixels with a dropped channel, %u in tile-sized hue jumps, %u non-finite (at most %u flagged)\n", argv[2], f.dropped, f.tiled,
            nonfinite, most);
        return f.total() <= most && nonfinite == 0 ? 0 : 1;
    }

    int chromatic_sweep(int argc, char **argv)
    {
        std::vector<std::filesystem::path> folders;
        std::error_code error;
        for (const auto &entry : std::filesystem::directory_iterator(argv[2], error))
            if (std::filesystem::exists(entry.path() / "frame.tsv"))
                folders.push_back(entry.path());
        std::sort(folders.begin(), folders.end());
        uint32_t worst = 0, nonfinite = 0;
        std::string worst_case = "none";
        for (const std::filesystem::path &folder : folders)
        {
            std::string why;
            const std::optional<FrameSave> frame = read_frame_folder(folder, why);
            if (!frame || !dof_depth_coefficients(frame->camera))
            {
                std::fprintf(stderr, "%s: %s\n", folder.c_str(), frame ? "the projection does not invert" : why.c_str());
                return 2;
            }
            const float p11 = frame->camera.view_to_clip[5];
            DepthOfFieldSettings base = user_settings();
            const std::string unknown = apply(base, p11, 3, argc, argv);
            if (!unknown.empty())
            {
                std::fprintf(stderr, "unknown argument %s\n", unknown.c_str());
                return 2;
            }
            const DofImage image = image_of(*frame);
            base.focus_distance = 0;
            const float autofocus = render_at(image, *frame, base).focus;
            uint32_t frame_worst = 0;
            std::string frame_case;
            for (const float mm : { 90.0f, 120.0f, 180.0f, 300.0f })
                for (const float times : { 1.0f, 0.98f, 1.02f, 0.85f, 1.3f })
                {
                    DepthOfFieldSettings s = base;
                    s.sensor_height_mm = sensor_for(mm, p11);
                    s.focus_distance = autofocus * times;
                    const ChromaticFlags f = flags_at(image, *frame, s, nonfinite);
                    char text[200];
                    std::snprintf(text, sizeof text, "%s mm=%g (sensor %.2f, %.0f mm) focus=%.4g", folder.filename().c_str(), mm,
                        clamp_depth_of_field(s).sensor_height_mm, clamp_depth_of_field(s).sensor_height_mm * 0.5f * p11, s.focus_distance);
                    std::printf("%s: %u dropped, %u tiled\n", text, f.dropped, f.tiled);
                    if (f.total() >= frame_worst)
                    {
                        frame_worst = f.total();
                        frame_case = text;
                    }
                }
            std::printf("frame %s: worst %u at %s\n", folder.filename().c_str(), frame_worst, frame_case.c_str());
            if (frame_worst >= worst)
            {
                worst = frame_worst;
                worst_case = frame_case;
            }
        }
        std::printf("worst of %zu frames: %u flagged at %s, %u non-finite\n", folders.size(), worst, worst_case.c_str(), nonfinite);
        return nonfinite == 0 ? 0 : 3;
    }

    struct Seam
    {
        float step = 0;     // tone curve levels, 0 to 255
        uint32_t x = 0, y = 0;  // the run's first pixel past the edge, in the picture from the top left
        bool vertical = true;
    };

    // The largest step of what the bokeh contrast changes across a run of 16 pixels along a line offset pixels past a
    // multiple of 16: offset 0 is a gather tile's edge (8 half-size pixels), offset 8 the middle of a tile, the control.
    // The far field's bilinear upsample spreads a half-size step over the line's pixel on either side, so in each row and
    // channel the step is the mean of the change d, in the tone curve's levels, over the 6 pixels after those less its mean
    // over the 6 before, less what the sides' own slope accounts for. A seam steps between flat sides, while the contrast
    // moves a disc's rim and the picture's own edges wherever they lie, and the far field steps with the pixels' own discs
    // across a step in depth even where the blur hides it. So a row counts only where, on either side and in every
    // channel, d varies by at most 1.5 levels and the render without the contrast by at most 3, and the depth by at most 1%
    // over the row. The run's step is the median of its rows' when 12 or more count, the worst channel's.
    Seam worst_seam(const std::vector<float> &plain, const std::vector<float> &contrast, const FrameSave &frame, uint32_t offset)
    {
        const int w = int(frame.size.width), h = int(frame.size.height);
        const std::array<float, 4> k = *dof_depth_coefficients(frame.camera);
        const auto range = [](const float *v) { return *std::max_element(v, v + 6) - *std::min_element(v, v + 6); };
        Seam worst;
        for (const bool vertical : { true, false })
        {
            const int across = vertical ? w : h, along = vertical ? h : w;
            for (int e = 16 + int(offset); e + 6 < across; e += 16)
                for (int run = 0; run + 16 <= along; run += 16)
                {
                    float steps[3][16];
                    int count = 0;
                    for (int at = run; at < run + 16; at++)
                    {
                        float d[3][14], p[3][14], nearest = INFINITY, farthest = 0;
                        bool flat = true;
                        for (int j = 0; j < 14; j++)
                        {
                            const int x = vertical ? e - 7 + j : at, y = vertical ? at : e - 7 + j;
                            for (int c = 0; c < 3; c++)
                            {
                                const size_t i = (size_t(y) * w + x) * 3 + c;
                                p[c][j] = 255 * tone(plain[i], 0.985f);
                                d[c][j] = 255 * tone(contrast[i], 0.985f) - p[c][j];
                            }
                            const float z = dof_view_depth(k, frame.depth[size_t(y) * w + x]);
                            nearest = std::min(nearest, z);
                            farthest = std::max(farthest, z);
                        }
                        for (int c = 0; c < 3; c++)
                            flat = flat && range(d[c]) <= 1.5f && range(d[c] + 8) <= 1.5f && range(p[c]) <= 3 && range(p[c] + 8) <= 3;
                        if (!flat || !(farthest <= nearest * 1.01f))
                            continue;
                        for (int c = 0; c < 3; c++)
                        {
                            const float *v = d[c];
                            const float left = std::accumulate(v, v + 6, 0.0f) / 6, right = std::accumulate(v + 8, v + 14, 0.0f) / 6;
                            steps[c][count] = (right - left) - ((v[5] - v[0]) + (v[13] - v[8])) / 10 * 8;
                        }
                        count++;
                    }
                    if (count < 12)
                        continue;
                    for (int c = 0; c < 3; c++)
                    {
                        std::nth_element(steps[c], steps[c] + count / 2, steps[c] + count);
                        const float step = std::fabs(steps[c][count / 2]);
                        if (step <= worst.step)
                            continue;
                        worst.step = step;
                        worst.vertical = vertical;
                        worst.x = uint32_t(vertical ? e : run);
                        const uint32_t y = uint32_t(vertical ? run : e);
                        worst.y = frame.flip ? uint32_t(h) - (vertical ? y + 16 : y) : y;
                    }
                }
        }
        return worst;
    }

    std::string seam_text(const Seam &s)
    {
        char text[120];
        std::snprintf(text, sizeof text, "%.2f levels on the %s edge at %u,%u", s.step, s.vertical ? "vertical" : "horizontal", s.x, s.y);
        return text;
    }

    // The tile edges' and the controls' worst seams of one setting, from renders with and without the bokeh contrast.
    std::pair<Seam, Seam> seams_at(const DofImage &image, const FrameSave &frame, DepthOfFieldSettings settings, uint32_t &nonfinite)
    {
        const DofOutput contrast = render_at(image, frame, settings);
        settings.bokeh_contrast = 0;
        const DofOutput plain = render_at(image, frame, settings);
        nonfinite += plain.nonfinite + contrast.nonfinite;
        return { worst_seam(plain.rgb, contrast.rgb, frame, 0), worst_seam(plain.rgb, contrast.rgb, frame, 8) };
    }

    int seam_check(int argc, char **argv)
    {
        std::string why;
        const std::optional<FrameSave> frame = read_frame_folder(argv[2], why);
        if (!frame || !dof_depth_coefficients(frame->camera))
        {
            std::fprintf(stderr, "%s: %s\n", argv[2], frame ? "the projection does not invert" : why.c_str());
            return 2;
        }
        DepthOfFieldSettings settings = user_settings();
        const std::string unknown = apply(settings, frame->camera.view_to_clip[5], 4, argc, argv);
        if (!unknown.empty())
        {
            std::fprintf(stderr, "unknown argument %s\n", unknown.c_str());
            return 2;
        }
        const float most = std::strtof(argv[3], nullptr);
        uint32_t nonfinite = 0;
        const auto [edge, control] = seams_at(image_of(*frame), *frame, settings, nonfinite);
        std::printf("%s: bokeh contrast seam %s (inside tiles %s), %u non-finite (at most %.2f)\n", argv[2], seam_text(edge).c_str(),
            seam_text(control).c_str(), nonfinite, most);
        return edge.step <= most && nonfinite == 0 ? 0 : 1;
    }

    int seam_sweep(int argc, char **argv)
    {
        std::vector<std::filesystem::path> folders;
        std::error_code error;
        for (const auto &entry : std::filesystem::directory_iterator(argv[2], error))
            if (std::filesystem::exists(entry.path() / "frame.tsv"))
                folders.push_back(entry.path());
        std::sort(folders.begin(), folders.end());
        float worst = 0, worst_control = 0;
        std::string worst_case = "none";
        uint32_t nonfinite = 0;
        for (const std::filesystem::path &folder : folders)
        {
            std::string why;
            const std::optional<FrameSave> frame = read_frame_folder(folder, why);
            if (!frame || !dof_depth_coefficients(frame->camera))
            {
                std::fprintf(stderr, "%s: %s\n", folder.c_str(), frame ? "the projection does not invert" : why.c_str());
                return 2;
            }
            const float p11 = frame->camera.view_to_clip[5];
            DepthOfFieldSettings base = user_settings();
            const std::string unknown = apply(base, p11, 3, argc, argv);
            if (!unknown.empty())
            {
                std::fprintf(stderr, "unknown argument %s\n", unknown.c_str());
                return 2;
            }
            const DofImage image = image_of(*frame);
            base.focus_distance = 0;
            const float autofocus = render_at(image, *frame, base).focus;
            for (const float mm : { 50.0f, 90.0f, 180.0f, 300.0f })
                for (const float times : { 1.0f, 0.85f, 1.3f })
                {
                    DepthOfFieldSettings s = base;
                    s.sensor_height_mm = sensor_for(mm, p11);
                    s.focus_distance = autofocus * times;
                    const auto [edge, control] = seams_at(image, *frame, s, nonfinite);
                    char text[160];
                    std::snprintf(text, sizeof text, "%s mm=%g focus=%.4g", folder.filename().c_str(), mm, s.focus_distance);
                    std::printf("%s: seam %s (inside tiles %.2f)\n", text, seam_text(edge).c_str(), control.step);
                    worst_control = std::max(worst_control, control.step);
                    if (edge.step >= worst)
                    {
                        worst = edge.step;
                        worst_case = std::string(text) + ": " + seam_text(edge);
                    }
                }
        }
        std::printf("worst of %zu frames: %s; inside tiles at most %.2f levels; %u non-finite\n", folders.size(), worst_case.c_str(), worst_control,
            nonfinite);
        return nonfinite == 0 ? 0 : 3;
    }
    // The near field's edge: a plane 1 m away over the left half of a kNearEdgeWidth x height frame through the synthetic
    // camera's 50 mm lens, focused at 10 m, over a flat plane at background meters, both flat gray, round aperture, no
    // aberration. A lens spreads the near plane's light over what lies behind by the share of each pixel's disc the plane
    // covers, so across the edge the picture falls from the near gray to the other with a disc's edge spread: from 90% to
    // 10% over 2 kDiscEdge10 times the near plane's CoC radius, whether the background is in focus or a little in front of
    // it. Measured, linear in both places, along the mean of the middle rows. The edge lies offset pixels right of the
    // frame's middle: past a whole pixel, the pixel it crosses blends the two grays by the share the plane covers, as the
    // game's SMAA or a supersampled shot leave an outline, while the depth holds the surface under its center.
    constexpr uint32_t kNearEdgeWidth = 512;
    constexpr float kNearEdgeMeters = 1, kNearEdgeFocus = 10;
    // The distance from a disc's center, over its radius, of the chord that cuts off a tenth of its area.
    constexpr float kDiscEdge10 = 0.68705f;
    // The most the edge's profile may rise from one pixel to the next, as a share of the step from the background's gray to
    // the near plane's: R16F's rounding and the gather's noise, which the 32 rows average.
    constexpr float kNearEdgeRise = 0.005f;
    // How much steeper than a disc's edge spread the profile may fall from one pixel to the next: a disc of radius r falls
    // by at most 2 / (pi r) a pixel, at its middle.
    constexpr float kNearEdgeSteep = 1.5f;

    struct NearEdge
    {
        float radius = 0;       // the near plane's capped CoC radius, full-size pixels
        float expected = 0;     // 10% to 90% width, full-size pixels
        float measured = 0;
        float rise = 0;         // the most the profile rises from one pixel to the next, where it must only fall
        float steep = 0;        // the most it falls from one pixel to the next, over a disc's steepest fall
        uint32_t nonfinite = 0;
    };

    NearEdge near_edge_at(DepthOfFieldSettings s, uint32_t height, float background, bool display, float offset)
    {
        const uint32_t w = kNearEdgeWidth, h = height;
        const Camera camera = synthetic_camera();
        const std::optional<std::array<float, 4>> k = dof_depth_coefficients(camera);
        const float near_gray = 1, far_gray = 0.1f;
        DofImage image;
        image.size = { w, h };
        image.rgb.resize(size_t(w) * h * 3);
        image.depth.resize(size_t(w) * h);
        for (uint32_t y = 0; y < h; y++)
            for (uint32_t x = 0; x < w; x++)
            {
                const float edge = float(w / 2) + offset;
                const float inverse = 1 / (float(x) + 0.5f < edge ? kNearEdgeMeters : background);
                const float cover = std::clamp(edge - float(x), 0.0f, 1.0f);
                image.depth[size_t(y) * w + x] = -((*k)[1] + inverse * (*k)[3]) / ((*k)[0] + inverse * (*k)[2]);
                for (int c = 0; c < 3; c++)
                    image.rgb[(size_t(y) * w + x) * 3 + c] = far_gray + (near_gray - far_gray) * cover;
            }
        s.focus_distance = kNearEdgeFocus;
        s.focus_seconds = 0;
        s = clamp_depth_of_field(s);
        const DofConstants c = display ? *dof_display_constants(s, camera, image.size, image.size, false, DofTransfer::srgb, 0)
                                       : *dof_constants(s, camera, image.size, false, 0, 0);
        // What the gray levels come back as when the picture's levels are unchanged.
        const auto stored = [display](float v) { return display ? dof_srgb_decode(std::round(dof_srgb_encode(v) * 255) / 255) : v; };
        DofOutput out;
        if (display)
        {
            std::vector<float> back(image.rgb.size());
            for (size_t i = 0; i < back.size(); i++)
                back[i] = std::round(dof_srgb_encode(image.rgb[i]) * 255) / 255;
            out = render_dof_display(back, image.depth, c, 0);
            for (float &v : out.rgb)
                v = dof_srgb_decode(v);
        }
        else
            out = render_dof(image, c, 0);
        const DofOptics optics { c.optics[0], c.optics[1], c.optics[2], 0, c.optics[3] };
        NearEdge e;
        e.radius = 2 * std::fabs(dof_capped_cocs(kNearEdgeMeters, kNearEdgeFocus, optics)[1]);
        e.expected = 2 * kDiscEdge10 * e.radius;
        e.nonfinite = out.nonfinite;
        const float lo = stored(far_gray), hi = stored(near_gray);
        std::vector<float> profile(w, 0.0f);
        const uint32_t rows = 32;
        for (uint32_t y = h / 2 - rows / 2; y < h / 2 + rows / 2; y++)
            for (uint32_t x = 0; x < w; x++)
                profile[x] += (out.rgb[(size_t(y) * w + x) * 3 + 1] - lo) / (hi - lo) / rows;
        // Where the profile first falls below level, between pixel centers.
        const auto crossing = [&profile](float level) {
            for (size_t x = 1; x < profile.size(); x++)
                if (profile[x] < level && profile[x - 1] >= level)
                    return float(x - 1) + (profile[x - 1] - level) / (profile[x - 1] - profile[x]);
            return 0.0f;
        };
        e.measured = crossing(0.1f) - crossing(0.9f);
        for (size_t x = 1; x < profile.size(); x++)
        {
            e.rise = std::max(e.rise, profile[x] - profile[x - 1]);
            e.steep = std::max(e.steep, (profile[x - 1] - profile[x]) / (2 / (3.14159265f * e.radius)));
        }
        return e;
    }

    // A farther surface in front of focus beside a nearer one: the plane 1 m away over the left kLayerNearShare of the
    // frame, and right of it a plane at kLayerFarther meters, also in front of focus, dark at first and light from three
    // of its CoC radii past the nearer plane's blur, which the tiles' largest near disc still reaches. A lens blurs that
    // edge with the farther plane's own disc, whatever lies nearer beside it, so it rises from 10% to 90% over
    // 2 kDiscEdge10 times that plane's CoC radius; blurred with the nearer plane's disc, it spread 3.9 times as wide.
    // Measured as near_edge_at measures.
    constexpr float kLayerNearShare = 0.2f, kLayerFarther = 3;

    NearEdge layer_edge_at(DepthOfFieldSettings s, uint32_t height, bool display)
    {
        const uint32_t w = kNearEdgeWidth, h = height;
        const Camera camera = synthetic_camera();
        const std::optional<std::array<float, 4>> k = dof_depth_coefficients(camera);
        const float near_gray = 1, dark = 0.1f, light = 0.6f;
        DofImage image;
        image.size = { w, h };
        s.focus_distance = kNearEdgeFocus;
        s.focus_seconds = 0;
        s = clamp_depth_of_field(s);
        const DofConstants c = display ? *dof_display_constants(s, camera, image.size, image.size, false, DofTransfer::srgb, 0)
                                       : *dof_constants(s, camera, image.size, false, 0, 0);
        const DofOptics optics { c.optics[0], c.optics[1], c.optics[2], 0, c.optics[3] };
        NearEdge e;
        e.radius = 2 * std::fabs(dof_capped_cocs(kLayerFarther, kNearEdgeFocus, optics)[1]);
        e.expected = 2 * kDiscEdge10 * e.radius;
        const float near_edge = kLayerNearShare * float(w);
        const float near_blur = near_edge + 2 * std::fabs(dof_capped_cocs(kNearEdgeMeters, kNearEdgeFocus, optics)[1]);
        const float edge = near_blur + 3 * e.radius;
        image.rgb.resize(size_t(w) * h * 3);
        image.depth.resize(size_t(w) * h);
        for (uint32_t y = 0; y < h; y++)
            for (uint32_t x = 0; x < w; x++)
            {
                const float at = float(x) + 0.5f;
                const bool near = at < near_edge;
                const float inverse = 1 / (near ? kNearEdgeMeters : kLayerFarther);
                image.depth[size_t(y) * w + x] = -((*k)[1] + inverse * (*k)[3]) / ((*k)[0] + inverse * (*k)[2]);
                for (int ch = 0; ch < 3; ch++)
                    image.rgb[(size_t(y) * w + x) * 3 + ch] = near ? near_gray : at < edge ? dark : light;
            }
        const auto stored = [display](float v) { return display ? dof_srgb_decode(std::round(dof_srgb_encode(v) * 255) / 255) : v; };
        DofOutput out;
        if (display)
        {
            std::vector<float> back(image.rgb.size());
            for (size_t i = 0; i < back.size(); i++)
                back[i] = std::round(dof_srgb_encode(image.rgb[i]) * 255) / 255;
            out = render_dof_display(back, image.depth, c, 0);
            for (float &v : out.rgb)
                v = dof_srgb_decode(v);
        }
        else
            out = render_dof(image, c, 0);
        e.nonfinite = out.nonfinite;
        const float lo = stored(dark), hi = stored(light);
        const uint32_t rows = 32;
        std::vector<float> profile(w, 0.0f);
        for (uint32_t y = h / 2 - rows / 2; y < h / 2 + rows / 2; y++)
            for (uint32_t x = 0; x < w; x++)
                profile[x] += (out.rgb[(size_t(y) * w + x) * 3 + 1] - lo) / (hi - lo) / rows;
        // Where the profile first rises past level right of the nearer plane's blur, between pixel centers.
        const size_t from = size_t(near_blur + 1);
        const auto crossing = [&profile, from](float level) {
            for (size_t x = from + 1; x < profile.size(); x++)
                if (profile[x] >= level && profile[x - 1] < level)
                    return float(x - 1) + (level - profile[x - 1]) / (profile[x] - profile[x - 1]);
            return 0.0f;
        };
        e.measured = crossing(0.9f) - crossing(0.1f);
        return e;
    }

    // Exits with 1 when an edge's width is off the disc's by more than the share most, or its profile rises anywhere by more
    // than kNearEdgeRise of the step (a bright line along the silhouette), or falls steeper than kNearEdgeSteep times a
    // disc's (a step along it), over a background in focus or in front of it, at
    // the live and the 4K screenshot's sizes, max blur 0.03 and 0.08, capped and not, in both places.
    int near_edge_check(int argc, char **argv)
    {
        DepthOfFieldSettings base;
        base.enabled = true;
        base.spherical_aberration = 0;
        base.samples = 289;
        const std::string unknown = apply(base, synthetic_camera().view_to_clip[5], 3, argc, argv);
        if (!unknown.empty())
        {
            std::fprintf(stderr, "unknown argument %s\n", unknown.c_str());
            return 2;
        }
        struct Case
        {
            uint32_t height;
            float max_blur, f_number, background;
            bool display;
            float offset = 0;
        };
        const Case cases[] = {
            { 800, 0.03f, 2.8f, kNearEdgeFocus, false }, { 800, 0.03f, 2.8f, 8, false }, { 800, 0.08f, 1, 5, true },
            { 2160, 0.03f, 1, 8, false }, { 2160, 0.03f, 2.8f, 5, false }, { 2160, 0.08f, 1, kNearEdgeFocus, true }, { 2160, 0.08f, 1, 8, true },
            { 2160, 0.03f, 2.8f, kNearEdgeFocus, true, 0.4f }, { 2160, 0.03f, 2.8f, kNearEdgeFocus, true, 0.7f }, { 800, 0.08f, 1, kNearEdgeFocus, true, 0.3f },
        };
        const float most = std::strtof(argv[2], nullptr);
        float worst = 0, worst_rise = 0, worst_steep = 0;
        uint32_t nonfinite = 0;
        for (const Case &k : cases)
        {
            DepthOfFieldSettings s = base;
            s.f_number = k.f_number;
            s.max_blur = k.max_blur;
            const NearEdge e = near_edge_at(s, k.height, k.background, k.display, k.offset);
            const float off = std::fabs(e.measured / e.expected - 1);
            const bool fails = off > most || e.rise > kNearEdgeRise || e.steep > kNearEdgeSteep;
            worst = std::max(worst, off);
            worst_rise = std::max(worst_rise, e.rise);
            worst_steep = std::max(worst_steep, e.steep);
            nonfinite += e.nonfinite;
            std::printf("height %u, max blur %.2f, f/%.1f, background %g m, %s, edge +%.1f px: CoC radius %.1f px, 10-90%% width %.1f px, the disc's %.1f, rising %.4f, steepest %.2f of a disc's%s\n", k.height,
                k.max_blur, k.f_number, k.background, k.display ? "display" : "scene", k.offset, e.radius, e.measured, e.expected, e.rise, e.steep, fails ? " FAIL" : "");
        }
        // The farther plane's edge beside the nearer one, at max blur 0.08, whose tiles reach past the nearer plane's blur.
        const Case layered[] = { { 2160, 0.08f, 2.8f, kLayerFarther, true }, { 800, 0.08f, 1, kLayerFarther, false } };
        for (const Case &k : layered)
        {
            DepthOfFieldSettings s = base;
            s.f_number = k.f_number;
            s.max_blur = k.max_blur;
            const NearEdge e = layer_edge_at(s, k.height, k.display);
            const float off = std::fabs(e.measured / e.expected - 1);
            worst = std::max(worst, off);
            nonfinite += e.nonfinite;
            std::printf("height %u, max blur %.2f, f/%.1f, a plane at %g m beside one at %g m, %s: its CoC radius %.1f px, its edge's 10-90%% width %.1f px, the disc's %.1f%s\n",
                k.height, k.max_blur, k.f_number, kLayerFarther, kNearEdgeMeters, k.display ? "display" : "scene", e.radius, e.measured, e.expected, off > most ? " FAIL" : "");
        }
        std::printf("near edges: worst %.1f%% off the disc's width (at most %.1f%%), rising %.4f (at most %.4f), steepest %.2f of a disc's (at most %.2f), %u non-finite\n",
            worst * 100, most * 100, worst_rise, kNearEdgeRise, worst_steep, kNearEdgeSteep, nonfinite);
        return worst <= most && worst_rise <= kNearEdgeRise && worst_steep <= kNearEdgeSteep && nonfinite == 0 ? 0 : 1;
    }

    // The far field's edge: a subject in focus at kFarEdgeFocus meters over the left of a kFarEdgeWidth x height back
    // buffer, its outline slanted by slope pixels a row, over a flat plane at kFarEdgeBackground meters far out of focus,
    // both flat gray, through the display place. The back buffer holds the outline anti-aliased, each pixel blending the
    // two grays by the share of it the subject covers, as the game's SMAA or a supersampled shot leave it, while the depth
    // holds whichever surface lies under the pixel's center. A lens leaves the subject and its anti-aliased outline as they
    // are and blurs only the plane behind, which is one gray: so every pixel should come back as it went in. Measured,
    // linear, as a share of the step between the grays: the most any pixel near the outline is off, and the most any
    // pixel more than kFarEdgeHalo pixels out from it is off, the subject's light spread over the background.
    constexpr uint32_t kFarEdgeWidth = 512;
    constexpr float kFarEdgeFocus = 2, kFarEdgeBackground = 50, kFarEdgeHalo = 1.5f;

    struct FarEdge
    {
        float edge = 0;     // the most a pixel within kFarEdgeHalo of the outline is off
        float halo = 0;     // the most a pixel past it on the background's side is off
        uint32_t nonfinite = 0;
    };

    FarEdge far_edge_at(DepthOfFieldSettings s, uint32_t height, float slope)
    {
        const uint32_t w = kFarEdgeWidth, h = height;
        const Camera camera = synthetic_camera();
        const std::optional<std::array<float, 4>> k = dof_depth_coefficients(camera);
        const float subject_gray = 1, plane_gray = 0.1f;
        const auto reversed = [&k](float meters) { return -((*k)[1] + (*k)[3] / meters) / ((*k)[0] + (*k)[2] / meters); };
        // The outline's x at a height y, in pixels, so the subject covers x < outline(y).
        const auto outline = [=](float y) { return float(w) / 2 + (y - float(h) / 2) * slope; };
        std::vector<float> back(size_t(w) * h * 3), depth(size_t(w) * h), cover(size_t(w) * h);
        for (uint32_t y = 0; y < h; y++)
            for (uint32_t x = 0; x < w; x++)
            {
                const int n = 16;
                int inside = 0;
                for (int j = 0; j < n; j++)
                    for (int i = 0; i < n; i++)
                        inside += float(x) + (i + 0.5f) / n < outline(float(y) + (j + 0.5f) / n);
                const size_t p = size_t(y) * w + x;
                cover[p] = float(inside) / (n * n);
                depth[p] = reversed(float(x) + 0.5f < outline(float(y) + 0.5f) ? kFarEdgeFocus : kFarEdgeBackground);
                const float linear = plane_gray + (subject_gray - plane_gray) * cover[p];
                for (int c = 0; c < 3; c++)
                    back[p * 3 + c] = std::round(dof_srgb_encode(linear) * 255) / 255;
            }
        s.focus_distance = kFarEdgeFocus;
        s.focus_seconds = 0;
        s = clamp_depth_of_field(s);
        const DofConstants c = *dof_display_constants(s, camera, { w, h }, { w, h }, false, DofTransfer::srgb, 0);
        DofOutput out = render_dof_display(back, depth, c, 0);
        FarEdge e;
        e.nonfinite = out.nonfinite;
        // The rows away from the frame's top and bottom, where the gather clamps.
        for (uint32_t y = h / 4; y < h - h / 4; y++)
            for (uint32_t x = 0; x < w; x++)
            {
                const size_t p = size_t(y) * w + x;
                const float stored = dof_srgb_decode(back[p * 3 + 1]);
                const float off = std::fabs(dof_srgb_decode(out.rgb[p * 3 + 1]) - stored) / (subject_gray - plane_gray);
                const float past = float(x) + 0.5f - outline(float(y) + 0.5f);
                if (std::fabs(past) <= kFarEdgeHalo)
                    e.edge = std::max(e.edge, off);
                else if (past > 0)
                    e.halo = std::max(e.halo, off);
            }
        return e;
    }

    // Exits with 1 when a pixel near the outline is off by more than the share most, or one past it by more than the share
    // halo, at the live and the 4K screenshot's sizes, through a lens wide open and stopped down.
    int far_edge_check(int argc, char **argv)
    {
        DepthOfFieldSettings base;
        base.enabled = true;
        base.spherical_aberration = 0;
        base.samples = 289;
        const std::string unknown = apply(base, synthetic_camera().view_to_clip[5], 4, argc, argv);
        if (!unknown.empty())
        {
            std::fprintf(stderr, "unknown argument %s\n", unknown.c_str());
            return 2;
        }
        struct Case
        {
            uint32_t height;
            float f_number, slope;
        };
        const Case cases[] = { { 800, 1.4f, 0.27f }, { 800, 4, 1.9f }, { 2160, 1.4f, 0.27f }, { 2160, 2.8f, 0.61f } };
        const float most = std::strtof(argv[2], nullptr), most_halo = std::strtof(argv[3], nullptr);
        float worst = 0, worst_halo = 0;
        uint32_t nonfinite = 0;
        for (const Case &k : cases)
        {
            DepthOfFieldSettings s = base;
            s.f_number = k.f_number;
            const FarEdge e = far_edge_at(s, k.height, k.slope);
            worst = std::max(worst, e.edge);
            worst_halo = std::max(worst_halo, e.halo);
            nonfinite += e.nonfinite;
            std::printf("height %u, f/%.1f, slope %.2f: outline off by %.4f, background past it by %.4f%s\n", k.height, k.f_number, k.slope, e.edge,
                e.halo, e.edge > most || e.halo > most_halo ? " FAIL" : "");
        }
        std::printf("far edges: outline off by at most %.4f (at most %.4f), background by %.4f (at most %.4f), %u non-finite\n", worst, most,
            worst_halo, most_halo, nonfinite);
        return worst <= most && worst_halo <= most_halo && nonfinite == 0 ? 0 : 1;
    }
}

int main(int argc, char **argv)
{
    if (argc == 3 && std::string(argv[1]) == "--synthetic")
        return synthetic(argv[2]);
    if (argc >= 3 && std::string(argv[1]) == "--chromatic-sweep")
        return chromatic_sweep(argc, argv);
    if (argc >= 4 && std::string(argv[1]) == "--chromatic-check")
        return chromatic_check(argc, argv);
    if (argc >= 3 && std::string(argv[1]) == "--seam-sweep")
        return seam_sweep(argc, argv);
    if (argc >= 4 && std::string(argv[1]) == "--seam-check")
        return seam_check(argc, argv);
    if (argc >= 3 && std::string(argv[1]) == "--near-edge-check")
        return near_edge_check(argc, argv);
    if (argc >= 4 && std::string(argv[1]) == "--far-edge-check")
        return far_edge_check(argc, argv);
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
    uint32_t frames = 1, first = 0;
    bool display = false;
    float exposure = 0.985f;
    int crop[4] = { 0, 0, int(frame->size.width), int(frame->size.height) };
    int zoom = 1, upscale = 1;
    float near_steps = 0, most_steps = 0;
    bool steps_failed = false;
    for (int i = 3; i < argc; i++)
    {
        const std::string arg = argv[i];
        const size_t eq = arg.find('=');
        const std::string key = arg.substr(0, eq), value = eq == std::string::npos ? "" : arg.substr(eq + 1);
        if (key == "frames")
            frames = std::max(1, std::atoi(value.c_str()));
        else if (key == "first")
            first = static_cast<uint32_t>(std::max(0, std::atoi(value.c_str())));
        else if (key == "display")
            display = std::atoi(value.c_str()) != 0;
        else if (key == "exposure")
            exposure = std::strtof(value.c_str(), nullptr);
        else if (key == "zoom")
            zoom = std::max(1, std::atoi(value.c_str()));
        else if (key == "upscale")
            upscale = std::max(1, std::atoi(value.c_str()));
        else if (key == "near_steps")
            near_steps = std::strtof(value.c_str(), nullptr);
        else if (key == "most_steps")
            most_steps = std::strtof(value.c_str(), nullptr);
        else if (key == "crop" && std::sscanf(value.c_str(), "%d,%d,%d,%d", &crop[0], &crop[1], &crop[2], &crop[3]) == 4)
            continue;
        else if (!apply(settings, frame->camera.view_to_clip[5], i, i + 1, argv).empty())
        {
            std::fprintf(stderr, "unknown argument %s\n", arg.c_str());
            return 2;
        }
    }
    settings = clamp_depth_of_field(settings);
    FrameSave fs = *frame;
    DofImage image = image_of(*frame);
    if (upscale > 1)
    {
        const uint32_t sw = frame->size.width, sh = frame->size.height, k = uint32_t(upscale);
        fs.size = { sw * k, sh * k };
        fs.depth.assign(size_t(sw) * sh * k * k, 0.0f);
        DofImage big;
        big.size = fs.size;
        big.depth.assign(fs.depth.size(), 0.0f);
        big.rgb.assign(fs.depth.size() * 3, 0.0f);
        for (uint32_t y = 0; y < sh * k; y++)
            for (uint32_t x = 0; x < sw * k; x++)
            {
                const size_t to = size_t(y) * sw * k + x;
                fs.depth[to] = big.depth[to] = frame->depth[size_t(y / k) * sw + x / k];
                const float u = (float(x) + 0.5f) / k - 0.5f, v = (float(y) + 0.5f) / k - 0.5f;
                const int x0 = std::clamp(int(std::floor(u)), 0, int(sw) - 1), y0 = std::clamp(int(std::floor(v)), 0, int(sh) - 1);
                const int x1 = std::min(x0 + 1, int(sw) - 1), y1 = std::min(y0 + 1, int(sh) - 1);
                const float fx = std::clamp(u - std::floor(u), 0.0f, 1.0f), fy = std::clamp(v - std::floor(v), 0.0f, 1.0f);
                for (int c = 0; c < 3; c++)
                {
                    const auto at = [&](int xx, int yy) { return image.rgb[(size_t(yy) * sw + xx) * 3 + c]; };
                    big.rgb[to * 3 + c] = (at(x0, y0) * (1 - fx) + at(x1, y0) * fx) * (1 - fy) + (at(x0, y1) * (1 - fx) + at(x1, y1) * fx) * fy;
                }
            }
        image = std::move(big);
    }
    const uint32_t w = fs.size.width, h = fs.size.height;
    // The display place's back buffer: the frame through the tone curve, quantized to 8 bits, upright.
    std::vector<float> back;
    if (display)
    {
        back.resize(image.rgb.size());
        for (uint32_t y = 0; y < h; y++)
            for (uint32_t x = 0; x < w; x++)
                for (int c = 0; c < 3; c++)
                    back[(size_t(y) * w + x) * 3 + c] = std::round(tone(image.rgb[(size_t(fs.flip ? h - 1 - y : y) * w + x) * 3 + c], exposure) * 255) / 255;
    }
    const auto start = std::chrono::steady_clock::now();
    // What the picture shows, 0 to 1 a channel, upright: the scene place's average through the tone curve, or the display
    // place's encoded average.
    std::vector<float> sum(image.rgb.size(), 0.0f);
    float focus = 0, luminance = 0;
    uint32_t sprites[2] = {}, nonfinite = 0;
    for (uint32_t f = first; f < first + frames; f++)
    {
        const std::optional<DofConstants> constants = display
            ? dof_display_constants(settings, fs.camera, fs.size, fs.size, fs.flip, DofTransfer::srgb, f)
            : dof_constants(settings, fs.camera, fs.size, fs.flip, 0, f);
        if (!constants)
        {
            std::fprintf(stderr, "the frame's projection does not invert\n");
            return 1;
        }
        const DofOutput out = display ? render_dof_display(back, fs.depth, *constants, focus) : render_dof(image, *constants, focus);
        focus = out.focus;
        luminance = out.luminance;
        sprites[0] = out.sprites[0];
        sprites[1] = out.sprites[1];
        nonfinite += out.nonfinite;
        for (size_t i = 0; i < sum.size(); i++)
            sum[i] += out.rgb[i] / frames;
    }
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    std::vector<float> shown(sum.size());
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++)
            for (int c = 0; c < 3; c++)
            {
                const size_t to = (size_t(y) * w + x) * 3 + c;
                shown[to] = display ? sum[to] : tone(sum[(size_t(fs.flip ? h - 1 - y : y) * w + x) * 3 + c], exposure);
            }
    if (near_steps > 0)
    {
        // The most two neighbors within the crop, both nearer than near_steps meters, differ by, in 8-bit levels of green:
        // inside a blurred foreground nothing jumps.
        const std::array<float, 4> k = *dof_depth_coefficients(fs.camera);
        const auto near = [&](uint32_t x, uint32_t y) {
            return dof_view_depth(k, fs.depth[size_t(fs.flip ? h - 1 - y : y) * w + x]) < near_steps;
        };
        float most = 0;
        uint32_t at[2] = { 0, 0 };
        for (uint32_t y = uint32_t(crop[1]); y + 1 < std::min(h, uint32_t(crop[1] + crop[3])); y++)
            for (uint32_t x = uint32_t(crop[0]); x + 1 < std::min(w, uint32_t(crop[0] + crop[2])); x++)
                if (near(x, y))
                    for (const auto [nx, ny] : { std::pair(x + 1, y), std::pair(x, y + 1) })
                        if (near(nx, ny))
                        {
                            const float step = std::fabs(shown[(size_t(ny) * w + nx) * 3 + 1] - shown[(size_t(y) * w + x) * 3 + 1]) * 255;
                            if (step > most)
                            {
                                most = step;
                                at[0] = x;
                                at[1] = y;
                            }
                        }
        std::printf("inside what lies nearer than %g m, neighbors differ by at most %.1f levels, at %u,%u\n", near_steps, most, at[0], at[1]);
        steps_failed = most_steps > 0 && most > most_steps;
    }
    crop[0] = std::clamp(crop[0], 0, int(w) - 1);
    crop[1] = std::clamp(crop[1], 0, int(h) - 1);
    crop[2] = std::clamp(crop[2], 1, int(w) - crop[0]);
    crop[3] = std::clamp(crop[3], 1, int(h) - crop[1]);
    const uint32_t ow = uint32_t(crop[2] * zoom), oh = uint32_t(crop[3] * zoom);
    std::vector<uint8_t> rgb(size_t(ow) * oh * 3);
    for (uint32_t y = 0; y < oh; y++)
        for (uint32_t x = 0; x < ow; x++)
        {
            const size_t at = (size_t(crop[1] + y / zoom) * w + crop[0] + x / zoom) * 3;
            for (int c = 0; c < 3; c++)
                rgb[(size_t(y) * ow + x) * 3 + c] = static_cast<uint8_t>(std::clamp(shown[at + c], 0.0f, 1.0f) * 255 + 0.5f);
        }
    std::ofstream(argv[2], std::ios::binary) << encode_png(rgb.data(), ow, oh);
    double energy = 0;
    for (const float v : sum)
        energy += v;
    std::printf("%s: focus %.4f m, mean luminance %.3f, %u far and %u near sprites, energy %.6f a channel, %u non-finite, %u frame%s in %.2f s\n",
        argv[2], focus, luminance, sprites[0], sprites[1], energy / double(sum.size()), nonfinite, frames, frames == 1 ? "" : "s", seconds);
    return nonfinite != 0 ? 3 : steps_failed ? 1 : 0;
}
