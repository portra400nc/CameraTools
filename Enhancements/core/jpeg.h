// A baseline JPEG encoder for the preview a DNG carries: 8-bit RGB to YCbCr with 4:2:0 chroma, the standard quantization
// tables scaled by quality as libjpeg scales them, and the standard Huffman tables. Pure C++17.
#pragma once

#include <cstdint>
#include <vector>

namespace enhancements
{
    // rgb holds width * height pixels, three bytes each, top row first. width and height from 1 to 65535, quality 1 to 100.
    std::vector<uint8_t> encode_jpeg(const uint8_t *rgb, uint32_t width, uint32_t height, int quality);
}
