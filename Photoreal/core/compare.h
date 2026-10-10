// The comparison capture's decisions: its variants, which present does what, back buffer pixels, flicker, and the rows
// of its files. Pure C++17 like plan.h. The shell copies, reads back and writes; this says when and what.
#pragma once

#include "plan.h"

#include <optional>
#include <string>
#include <vector>

struct PhotorealVariant;  // photoreal.h, read only by parse_variants

namespace photoreal
{
    constexpr uint32_t kCompareMax = 16;
    // Presents a variant's settings run before its first frame is copied.
    constexpr uint32_t kSettlePresents = 4;
    // Presents between a variant's second copy and reading both back, so the GPU has finished the copies and mapping
    // them does not wait. The read comes before the next variant's first copy reuses the staging texture.
    constexpr uint32_t kReadDelay = 2;
    static_assert(kReadDelay <= kSettlePresents);

    struct Variant
    {
        std::string name;   // only A-Z, a-z, 0-9, '.', '_' and '-', so it is a file name and a TSV cell
        Settings settings;
    };

    // Each name is cut at its first NUL or 32 bytes, and any other byte becomes '-'. Empty when the input is invalid:
    // null, no variants, more than kCompareMax, an empty name, or settings too short to hold enabled.
    std::optional<std::vector<Variant>> parse_variants(const PhotorealVariant *raw, uint32_t count);

    // What the shell does at one present of a capture, counted from 0 at the present that starts it, in field order.
    // Variant v's settings apply at present 6v. Its frames are copied at 6v + 5 and 6v + 6, the two presents after the
    // 4 settling ones, and read back at 6v + 8. The next variant applies at the second copy, and after the last one the
    // settings from before the capture come back.
    struct CompareTick
    {
        struct Copy
        {
            uint32_t variant;
            uint32_t frame;     // 0 or 1, also the staging slot
        };
        std::optional<Copy> copy;       // copy the back buffer before ReShade's effects
        std::optional<uint32_t> read;   // read this variant's two copies and hand them to the writer
        std::optional<uint32_t> apply;  // run this variant's settings from the next frame
        bool restore = false;           // run the settings from before the capture from the next frame
        bool done = false;              // the capture ends after this present
    };

    CompareTick compare_tick(uint32_t present, uint32_t count);

    enum class PixelOrder : uint8_t { rgba, bgra };

    // A back buffer as read back: rows of 4 bytes a pixel with no padding, top row first.
    struct RawFrame
    {
        Size size;
        PixelOrder order = PixelOrder::rgba;
        std::vector<uint8_t> bytes;
    };

    // Three bytes a pixel, alpha dropped. sRGB formats need nothing: their bytes are already what a PNG holds.
    std::vector<uint8_t> to_rgb(const RawFrame &frame);

    // How two frames of one variant differ. A pixel's difference is its largest channel difference.
    struct Flicker
    {
        double changed = 0;     // fraction of pixels whose difference is over 2
        uint32_t max = 0;
        double mean = 0;
    };

    // a and b are RGB images of one size.
    Flicker flicker(const std::vector<uint8_t> &a, const std::vector<uint8_t> &b);

    // "03-ambient.png"
    std::string png_name(uint32_t index, const std::string &name);

    struct CompareRow
    {
        uint32_t index;
        std::string name;
        std::string line;   // describe() for the variant's second frame
        Flicker flicker;
        Size size;
    };

    // compare.tsv and settings.tsv, each with its header line.
    std::string compare_tsv(const std::vector<CompareRow> &rows);
    std::string settings_tsv(const std::vector<Variant> &variants);
}
