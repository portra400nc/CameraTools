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
// to write a magnified part; mm sets the sensor that gives that focal length on the frame's projection where a full-frame
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
// --near-edge-check renders a blurred plane's edge over a background in focus and in front of it (near_edge_at) and exits
// with 1 when its 10-90% width is off a disc's edge spread by more than the share most.
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
    // it. Measured, linear in both places, along the mean of the middle rows.
    constexpr uint32_t kNearEdgeWidth = 512;
    constexpr float kNearEdgeMeters = 1, kNearEdgeFocus = 10;
    // The distance from a disc's center, over its radius, of the chord that cuts off a tenth of its area.
    constexpr float kDiscEdge10 = 0.68705f;
    // The most the edge's profile may rise from one pixel to the next, as a share of the step from the background's gray to
    // the near plane's: R16F's rounding and the gather's noise, which the 32 rows average.
    constexpr float kNearEdgeRise = 0.005f;

    struct NearEdge
    {
        float radius = 0;       // the near plane's capped CoC radius, full-size pixels
        float expected = 0;     // 10% to 90% width, full-size pixels
        float measured = 0;
        float rise = 0;         // the most the profile rises from one pixel to the next, where it must only fall
        uint32_t nonfinite = 0;
    };

    NearEdge near_edge_at(DepthOfFieldSettings s, uint32_t height, float background, bool display)
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
                const bool near = x < w / 2;
                const float inverse = 1 / (near ? kNearEdgeMeters : background);
                image.depth[size_t(y) * w + x] = -((*k)[1] + inverse * (*k)[3]) / ((*k)[0] + inverse * (*k)[2]);
                for (int c = 0; c < 3; c++)
                    image.rgb[(size_t(y) * w + x) * 3 + c] = near ? near_gray : far_gray;
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
            e.rise = std::max(e.rise, profile[x] - profile[x - 1]);
        return e;
    }

    // Exits with 1 when an edge's width is off the disc's by more than the share most, or its profile rises anywhere by more
    // than kNearEdgeRise of the step (a bright line along the silhouette), over a background in focus or in front of it, at
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
        };
        const Case cases[] = {
            { 800, 0.03f, 2.8f, kNearEdgeFocus, false }, { 800, 0.03f, 2.8f, 8, false }, { 800, 0.08f, 1, 5, true },
            { 2160, 0.03f, 1, 8, false }, { 2160, 0.03f, 2.8f, 5, false }, { 2160, 0.08f, 1, kNearEdgeFocus, true }, { 2160, 0.08f, 1, 8, true },
        };
        const float most = std::strtof(argv[2], nullptr);
        float worst = 0, worst_rise = 0;
        uint32_t nonfinite = 0;
        for (const Case &k : cases)
        {
            DepthOfFieldSettings s = base;
            s.f_number = k.f_number;
            s.max_blur = k.max_blur;
            const NearEdge e = near_edge_at(s, k.height, k.background, k.display);
            const float off = std::fabs(e.measured / e.expected - 1);
            const bool fails = off > most || e.rise > kNearEdgeRise;
            worst = std::max(worst, off);
            worst_rise = std::max(worst_rise, e.rise);
            nonfinite += e.nonfinite;
            std::printf("height %u, max blur %.2f, f/%.1f, background %g m, %s: CoC radius %.1f px, 10-90%% width %.1f px, the disc's %.1f, rising %.4f%s\n", k.height,
                k.max_blur, k.f_number, k.background, k.display ? "display" : "scene", e.radius, e.measured, e.expected, e.rise, fails ? " FAIL" : "");
        }
        std::printf("near edges: worst %.1f%% off the disc's width (at most %.1f%%), rising %.4f (at most %.4f), %u non-finite\n", worst * 100,
            most * 100, worst_rise, kNearEdgeRise, nonfinite);
        return worst <= most && worst_rise <= kNearEdgeRise && nonfinite == 0 ? 0 : 1;
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
    int zoom = 1;
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
        else if (key == "crop" && std::sscanf(value.c_str(), "%d,%d,%d,%d", &crop[0], &crop[1], &crop[2], &crop[3]) == 4)
            continue;
        else if (!apply(settings, frame->camera.view_to_clip[5], i, i + 1, argv).empty())
        {
            std::fprintf(stderr, "unknown argument %s\n", arg.c_str());
            return 2;
        }
    }
    settings = clamp_depth_of_field(settings);
    const DofImage image = image_of(*frame);
    const uint32_t w = frame->size.width, h = frame->size.height;
    // The display place's back buffer: the frame through the tone curve, quantized to 8 bits, upright.
    std::vector<float> back;
    if (display)
    {
        back.resize(image.rgb.size());
        for (uint32_t y = 0; y < h; y++)
            for (uint32_t x = 0; x < w; x++)
                for (int c = 0; c < 3; c++)
                    back[(size_t(y) * w + x) * 3 + c] = std::round(tone(image.rgb[(size_t(frame->flip ? h - 1 - y : y) * w + x) * 3 + c], exposure) * 255) / 255;
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
            ? dof_display_constants(settings, frame->camera, frame->size, frame->size, frame->flip, DofTransfer::srgb, f)
            : dof_constants(settings, frame->camera, frame->size, frame->flip, 0, f);
        if (!constants)
        {
            std::fprintf(stderr, "the frame's projection does not invert\n");
            return 1;
        }
        const DofOutput out = display ? render_dof_display(back, frame->depth, *constants, focus) : render_dof(image, *constants, focus);
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
                shown[to] = display ? sum[to] : tone(sum[(size_t(frame->flip ? h - 1 - y : y) * w + x) * 3 + c], exposure);
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
    return nonfinite == 0 ? 0 : 3;
}
