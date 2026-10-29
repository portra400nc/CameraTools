// Writes the DNG of a frame EnhancementsSaveFrame saved: its HDR scene before any pass of ours, through core/dng.cpp, upright
// as the add-on writes a screenshot's. The folder holds no bloom, grade or flare, so the file shows the scene alone.
//
//     frame_dng <frame folder> <out.dng> [exif]
//
// With exif, the file says it was taken at 85 mm, f/1.4, focused at 2.35 m on 2026:10:29 11:30:00, as the prototype's test file does.
#include "../core/dng.h"
#include "../core/dof.h"
#include "../core/light.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace enhancements;

int main(int argc, char **argv)
{
    if (argc != 3 && !(argc == 4 && std::string(argv[3]) == "exif"))
    {
        std::fprintf(stderr, "usage: %s <frame folder> <out.dng> [exif]\n", argv[0]);
        return 2;
    }
    std::string why;
    const std::optional<FrameSave> frame = read_frame_folder(argv[1], why);
    if (!frame)
    {
        std::fprintf(stderr, "%s: %s\n", argv[1], why.c_str());
        return 1;
    }
    std::vector<float> rgb;
    rgb.reserve(frame->hdr.size() * 3);
    float largest = 0;
    for (const uint32_t stored : frame->hdr)
        for (const float v : hdr_rgb(stored))
        {
            rgb.push_back(v);
            largest = std::max(largest, v);
        }
    const DngExif exif = argc == 4 ? DngExif { 85.0f, 1.4f, 2.35f, "2026:10:29 11:30:00" } : DngExif {};
    if (!write_dng(argv[2], { rgb.data(), frame->size.width, frame->size.height, 3, frame->flip }, exif))
    {
        std::fprintf(stderr, "%s could not be written\n", argv[2]);
        return 1;
    }
    std::printf("%s: %ux%u, brightest channel %.3f, headroom %.0f\n", argv[2], frame->size.width, frame->size.height, largest, dng_headroom(largest));
    return 0;
}
