// A minimal PNG encoder for the comparison capture: 8-bit RGB, no row filter, and stored (uncompressed) deflate
// blocks, so the add-on needs no compression library. Pure C++17.
#pragma once

#include <cstdint>
#include <string>

namespace photoreal
{
    // rgb holds width * height pixels, three bytes each, top row first. width and height must not be 0.
    std::string encode_png(const uint8_t *rgb, uint32_t width, uint32_t height);
}
