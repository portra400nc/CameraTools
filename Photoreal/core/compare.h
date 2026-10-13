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

    // Presents a variant waits for the trace pass to reach its samples before any of its passes has come in, and the least
    // it waits in all: about a minute at 30 fps. Build 203 traced 64 passes in about 12 s.
    constexpr uint32_t kTraceWaitPresents = 1800;
    // The most a variant waits, however its trace goes: twenty minutes at 30 fps.
    constexpr uint32_t kTraceWaitMost = 36000;

    // The presents a variant may wait for its trace's target passes, from the pace it has shown: it had first passes when
    // its wait began, has passes now, and the newest of them came in after pass_presents presents of waiting. With none
    // gained yet that is kTraceWaitPresents; else pass_presents and twice what the passes still missing need at that pace,
    // at least kTraceWaitPresents and at most kTraceWaitMost. The pace is taken at a pass's end, so a trace that stops
    // advancing runs into the limit instead of raising it. Build 209's bounce-hit-ambient-0 was cut off at 94 of 128
    // passes after 1,800 presents, 19 presents a pass; this gives it 3,103.
    uint32_t trace_wait_limit(uint32_t first, uint32_t passes, uint32_t pass_presents, uint32_t target);

    // Whether the capture holds at present, as compare_tick counts presents, instead of ticking: present is a variant's
    // first copy, the variant traces and its trace has fewer samples than it asks for (short), and it has waited fewer than
    // limit presents (trace_wait_limit). A variant that changes the trace's settings starts its samples again, and without
    // the wait it would be saved with 1 to 5 of them. The shell counts waited and does not advance present while it holds.
    bool compare_waits(uint32_t present, uint32_t count, uint32_t waited, bool short_of_samples, uint32_t limit = kTraceWaitPresents);

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
        uint32_t index = 0;
        std::string name;
        std::string line;   // describe() for the variant's second frame
        Flicker flicker {};
        Size size {};
        // The trace pass of a variant that traces a ready scene: its full passes at the saved frame and the samples it asks
        // for (trace_target in core/trace.h), else empty; the presents it waited before its first copy, and
        // whether it stopped waiting at its limit, short of its samples, which compare.tsv's status then says too.
        std::optional<uint32_t> trace_samples, trace_target;
        uint32_t trace_wait = 0;
        bool trace_timeout = false;
    };

    // compare.tsv and settings.tsv, each with its header line.
    std::string compare_tsv(const std::vector<CompareRow> &rows);
    std::string settings_tsv(const std::vector<Variant> &variants);
}
