// The PNG encoder: the exact bytes of a tiny image, and a large image for check_png.py to decode with Python's zlib.
#include "../core/png.h"

#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

using namespace photoreal;

namespace
{
    int failures = 0;

    void check(const char *name, bool ok)
    {
        std::printf("%s %s\n", ok ? "PASS" : "FAIL", name);
        failures += ok ? 0 : 1;
    }
}

int main(int argc, char **argv)
{
    {
        const uint8_t rgb[] = { 255, 0, 0, 0, 255, 0, 0, 0, 255, 255, 255, 255 };
        const uint8_t expected[] = {
            0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x02,
            0x00, 0x00, 0x00, 0x02, 0x08, 0x02, 0x00, 0x00, 0x00, 0xfd, 0xd4, 0x9a, 0x73, 0x00, 0x00, 0x00, 0x19, 0x49, 0x44, 0x41,
            0x54, 0x78, 0x01, 0x01, 0x0e, 0x00, 0xf1, 0xff, 0x00, 0xff, 0x00, 0x00, 0x00, 0xff, 0x00, 0x00, 0x00, 0x00, 0xff, 0xff,
            0xff, 0xff, 0x1f, 0xee, 0x05, 0xfb, 0xde, 0xdd, 0xec, 0x2b, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42,
            0x60, 0x82,
        };
        check("a 2x2 red, green, blue and white image encodes to the bytes Python's zlib computes",
            encode_png(rgb, 2, 2) == std::string(reinterpret_cast<const char *>(expected), sizeof expected));
    }
    if (argc > 1)
    {
        // 300x200 makes 180200 bytes of scanlines, so the IDAT holds three stored blocks. check_png.py knows the pattern.
        const uint32_t width = 300, height = 200;
        std::vector<uint8_t> rgb(width * height * 3);
        for (uint32_t y = 0; y < height; y++)
            for (uint32_t x = 0; x < width; x++)
            {
                uint8_t *p = &rgb[(y * width + x) * 3];
                p[0] = static_cast<uint8_t>(x);
                p[1] = static_cast<uint8_t>(y);
                p[2] = static_cast<uint8_t>(x * y);
            }
        const std::string png = encode_png(rgb.data(), width, height);
        std::ofstream(std::string(argv[1]) + "/roundtrip.png", std::ios::binary).write(png.data(), static_cast<std::streamsize>(png.size()));
    }
    std::printf("%s: %d failed\n", __FILE__, failures);
    return failures == 0 ? 0 : 1;
}
