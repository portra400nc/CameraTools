// The lens flare on the Mac: draws core/flare.cpp's flare, which runs shaders/flare.hlsli's own code, over a frame
// EnhancementsSaveFrame saved, as the display place sees it (through the game's tone curve into an 8-bit sRGB back buffer),
// and writes a PNG, so the look can be tuned against reference photos before a Deck round.
//
//     render_flare <frame folder | picture.ppm | black | gray> <out.png> sun=<u>,<v> [key=value ...]
//
// sun is the source, 0 to 1 from the top left, and may lie off the picture. A binary PPM (P6, 8-bit sRGB) is drawn over
// as it is; black and gray draw over a plain 1280x800 picture. Keys: strength, arcs, filled (the filled arcs), rainbow, ghosts, streaks, glare, size, warmth, chromatic, visibility, exposure, and width and height
// for black and gray.
#include "../core/dof.h"
#include "../core/flare.h"
#include "../core/light.h"
#include "../core/png.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
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
}

int main(int argc, char **argv)
{
    if (argc < 3)
    {
        std::fprintf(stderr, "usage: render_flare <frame folder | black | gray> <out.png> sun=<u>,<v> [key=value ...]\n");
        return 2;
    }
    FlareSettings settings;
    settings.enabled = true;
    float sun_u = 0.8f, sun_v = 0.12f, visibility = 1, exposure = 0.985f;
    uint32_t w = 1280, h = 800;
    for (int i = 3; i < argc; i++)
    {
        const std::string arg = argv[i];
        const size_t eq = arg.find('=');
        const std::string key = arg.substr(0, eq), value = eq == std::string::npos ? "" : arg.substr(eq + 1);
        const float v = std::strtof(value.c_str(), nullptr);
        if (key == "sun" && std::sscanf(value.c_str(), "%f,%f", &sun_u, &sun_v) == 2) continue;
        else if (key == "strength") settings.strength = v;
        else if (key == "arcs") settings.arcs = v;
        else if (key == "filled") settings.filled_arcs = v;
        else if (key == "rainbow") settings.rainbow = v;
        else if (key == "ghosts") settings.ghosts = v;
        else if (key == "streaks") settings.streaks = v;
        else if (key == "glare") settings.glare = v;
        else if (key == "size") settings.size = v;
        else if (key == "warmth") settings.warmth = v;
        else if (key == "chromatic") settings.chromatic = v;
        else if (key == "visibility") visibility = v;
        else if (key == "exposure") exposure = v;
        else if (key == "width") w = uint32_t(v);
        else if (key == "height") h = uint32_t(v);
        else
        {
            std::fprintf(stderr, "unknown argument %s\n", arg.c_str());
            return 2;
        }
    }
    settings = clamp_flare(settings);
    const std::string source = argv[1];
    // The back buffer's linear color, upright.
    std::vector<float> linear;
    if (source == "black" || source == "gray")
        linear.assign(size_t(w) * h * 3, source == "black" ? 0.0f : 0.05f);
    else if (source.size() > 4 && source.substr(source.size() - 4) == ".ppm")
    {
        std::ifstream in(source, std::ios::binary);
        std::string magic;
        int max = 0;
        in >> magic >> w >> h >> max;
        in.get();
        std::vector<uint8_t> bytes(size_t(w) * h * 3);
        in.read(reinterpret_cast<char *>(bytes.data()), std::streamsize(bytes.size()));
        if (magic != "P6" || max != 255 || !in)
        {
            std::fprintf(stderr, "%s: not an 8-bit binary PPM\n", source.c_str());
            return 1;
        }
        linear.resize(bytes.size());
        for (size_t i = 0; i < bytes.size(); i++)
            linear[i] = dof_srgb_decode(bytes[i] / 255.0f);
    }
    else
    {
        std::string why;
        const std::optional<FrameSave> frame = read_frame_folder(source, why);
        if (!frame)
        {
            std::fprintf(stderr, "%s: %s\n", source.c_str(), why.c_str());
            return 1;
        }
        w = frame->size.width;
        h = frame->size.height;
        linear.resize(size_t(w) * h * 3);
        for (uint32_t y = 0; y < h; y++)
            for (uint32_t x = 0; x < w; x++)
            {
                const Vec3f c = hdr_rgb(frame->hdr[size_t(frame->flip ? h - 1 - y : y) * w + x]);
                for (int k = 0; k < 3; k++)
                    linear[(size_t(y) * w + x) * 3 + k] = dof_srgb_decode(std::round(tone(c[k], exposure) * 255) / 255);
            }
    }
    const FlareConstants constants = flare_constants(settings, flare_point(sun_u, sun_v, float(w) / float(h)), { w, h }, true);
    const std::vector<float> shown = render_flare(linear, constants, visibility);
    std::vector<uint8_t> rgb(shown.size());
    for (size_t i = 0; i < shown.size(); i++)
        rgb[i] = static_cast<uint8_t>(std::clamp(dof_srgb_encode(shown[i]), 0.0f, 1.0f) * 255 + 0.5f);
    std::ofstream(argv[2], std::ios::binary) << encode_png(rgb.data(), w, h);
    std::printf("%s: %ux%u, sun at %.2f, %.2f\n", argv[2], w, h, sun_u, sun_v);
    return 0;
}
