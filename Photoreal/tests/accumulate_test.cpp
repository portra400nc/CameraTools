// The accumulator's decisions: parsing PhotorealAccumulate and the camera's lens sample, which frames add, clear and
// present, the cat's-eye weight, and the depth read's pixel and meters.
#include "../core/accumulate.h"
#include "../photoreal.h"

#include <cmath>
#include <cstdio>
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

    bool near(float a, float b, float tolerance = 1e-4f) { return std::fabs(a - b) <= tolerance; }

    struct Frame
    {
        AccumulateMode mode;
        uint32_t generation;
        LensSample lens;
        bool fresh;
        Size render;
    };

    // Runs the frames through accumulate_tick as the shell does, committing each tick, and writes what each did and the
    // samples after it, such as "clear 0", "add 1" or "- 1".
    std::vector<std::string> run(const std::vector<Frame> &frames)
    {
        std::vector<std::string> out;
        Accumulator state;
        for (const Frame &f : frames)
        {
            AccumulateSettings settings;
            settings.mode = f.mode;
            settings.generation = f.generation;
            const AccumulateTick tick = accumulate_tick(state, settings, f.lens, f.fresh, f.render);
            std::string what;
            if (tick.clear)
                what += " clear";
            if (tick.add)
                what += " add";
            if (tick.present)
                what += " present";
            out.push_back((what.empty() ? "-" : what.substr(1)) + " " + std::to_string(tick.next.samples));
            state = tick.next;
        }
        return out;
    }

    const Size kSmall { 1152, 720 };
    const Size kLarge { 1920, 1200 };
    constexpr AccumulateMode kAdd = AccumulateMode::add, kPresent = AccumulateMode::present, kOff = AccumulateMode::off;

    LensSample marked(uint32_t index) { return { 0.5f, -0.25f, index, true }; }
    LensSample held(uint32_t index) { return { 0.5f, -0.25f, index, false }; }

    // Unity's GL.GetGPUProjectionMatrix for a 60 degree, 16:10 camera with near 0.1 and far 1000, reversed Z.
    Camera camera_60()
    {
        Camera c {};
        const float view_to_clip[16] = { 1.0825318f, 0, 0, 0, 0, 1.7320508f, 0, 0, 0, 0, 0.0001f, -1, 0, 0, 0.1f, 0 };
        for (int i = 0; i < 16; i++)
            c.view_to_clip[i] = view_to_clip[i];
        return c;
    }
}

int main()
{
    {
        const std::vector<std::string> ticks = run({
            { kAdd, 1, held(0), true, kSmall },
            { kAdd, 1, marked(0), true, kSmall },
            { kAdd, 1, marked(0), true, kSmall },
            { kAdd, 1, held(1), true, kSmall },
            { kAdd, 1, marked(1), false, kSmall },
            { kAdd, 1, marked(1), true, kSmall },
            { kPresent, 1, held(1), true, kSmall },
            { kPresent, 2, held(1), true, kSmall },
            { kAdd, 2, marked(1), true, kSmall },
            { kAdd, 2, marked(2), true, kLarge },
            { kOff, 2, marked(3), true, kLarge },
            { kPresent, 2, held(3), true, kLarge },
        });
        check("a new generation clears before any sample; a marked fresh frame adds once per sample; an unmarked or stale-camera "
              "frame adds nothing; present mode presents its own generation only; a new generation or render size clears and "
              "adds; off keeps the sum",
            ticks == std::vector<std::string> { "clear 0", "add 1", "- 1", "- 1", "- 1", "add 2", "present 2", "- 2", "clear add 1",
                "clear add 1", "- 1", "present 1" });
    }
    {
        const std::vector<std::string> ticks = run({ { kAdd, 3, held(0), true, kSmall }, { kPresent, 3, held(0), true, kSmall } });
        check("an empty sum is not presented", ticks == std::vector<std::string> { "clear 0", "- 0" });
    }
    {
        check("the center pixel sees a lens point 0.58 out fully with cat's eye 0.4", cat_eye_weight(0.5f, 0.3f, 0, 0, 1.6f, 0.4f, 0.1f) == 1.0f);
        check("a 16:10 corner with cat's eye 0.4 cuts the lens point (-0.6, -0.4), 1.12 from its center (0.339, 0.212)",
            cat_eye_weight(-0.6f, -0.4f, 1, 1, 1.6f, 0.4f, 0.1f) == 0.0f);
        check("the lens point (-0.5, -0.3), 0.983 from that center, falls in the soft edge at 0.0764",
            near(cat_eye_weight(-0.5f, -0.3f, 1, 1, 1.6f, 0.4f, 0.1f), 0.0764f));
        check("the opposite corner sees the same lens point fully", cat_eye_weight(-0.5f, -0.3f, -1, -1, 1.6f, 0.4f, 0.1f) == 1.0f);
        check("cat's eye 0 sees it fully at the corner", cat_eye_weight(-0.5f, -0.3f, 1, 1, 1.6f, 0.0f, 0.1f) == 1.0f);
        check("a lens point 0.95 out is halfway through the 0.1 edge", near(cat_eye_weight(0.95f, 0, 0, 0, 1.6f, 0.4f, 0.1f), 0.5f));
    }
    {
        check("the screen point (0.5, 0.25) is stored at (960, 900) in a flipped 1920x1200 target",
            depth_pixel(kLarge, 0.5f, 0.25f, true) == Pixel { 960, 900 });
        check("and at (960, 300) upright", depth_pixel(kLarge, 0.5f, 0.25f, false) == Pixel { 960, 300 });
        check("the right and bottom edges land on the last pixel", depth_pixel(kLarge, 1, 1, false) == Pixel { 1919, 1199 });
        check("a point off the screen or NaN has no pixel", !depth_pixel(kLarge, -0.1f, 0.5f, true) && !depth_pixel(kLarge, 0.5f, NAN, true)
            && !depth_pixel({}, 0.5f, 0.5f, true));
    }
    {
        const Camera c = camera_60();
        // 10 m ahead: clip z = 0.0001 * -10 + 0.1 = 0.099 over w = 10.
        const std::optional<float> center = view_depth(c, 0.5f, 0.5f, 0.0099f);
        const std::optional<float> side = view_depth(c, 0.2f, 0.9f, 0.0099f);
        check("reversed depth 0.0099 is 10 m ahead at the center and off it", center && near(*center, 10, 1e-3f) && side && near(*side, 10, 1e-3f));
        const std::optional<float> nearest = view_depth(c, 0.5f, 0.5f, 1);
        check("reversed depth 1 is the 0.1 m near plane", nearest && near(*nearest, 0.1f, 1e-5f));
        Camera singular {};
        check("a projection that does not invert gives no depth", !view_depth(singular, 0.5f, 0.5f, 0.5f));
    }
    {
        PhotorealAccumulate raw { sizeof raw, PHOTOREAL_ACCUMULATE_ADD, 7, 0.4f, 0.2f };
        const AccumulateSettings whole = parse_accumulate(raw);
        check("a whole struct reads add, generation 7, cat's eye 0.4, falloff 0.2", whole.mode == AccumulateMode::add
            && whole.generation == 7 && whole.cat_eye == 0.4f && whole.cat_eye_falloff == 0.2f);
        raw.mode = 9;
        raw.cat_eye = NAN;
        raw.cat_eye_falloff = INFINITY;
        const AccumulateSettings bad = parse_accumulate(raw);
        check("mode 9 reads as off, and NaN and infinity fall back to cat's eye 0 and falloff 0.1", bad.mode == AccumulateMode::off
            && bad.cat_eye == 0.0f && bad.cat_eye_falloff == 0.1f);
        raw.mode = PHOTOREAL_ACCUMULATE_PRESENT;
        raw.cat_eye = 2;
        raw.cat_eye_falloff = 0;
        const AccumulateSettings high = parse_accumulate(raw);
        check("present mode; cat's eye 2 clamps to 1 and falloff 0 to 0.01", high.mode == AccumulateMode::present && high.cat_eye == 1.0f
            && high.cat_eye_falloff == 0.01f);
        raw.size = 8;
        const AccumulateSettings short_struct = parse_accumulate(raw);
        check("an 8-byte struct reads its mode and keeps generation 0, cat's eye 0 and falloff 0.1",
            short_struct.mode == AccumulateMode::present && short_struct.generation == 0 && short_struct.cat_eye == 0.0f
                && short_struct.cat_eye_falloff == 0.1f);
        raw.size = 4;
        check("a struct too short for mode reads as off", parse_accumulate(raw).mode == AccumulateMode::off);
    }
    {
        PhotorealCamera raw {};
        raw.lens_sample[0] = 0.3f;
        raw.lens_sample[1] = -0.4f;
        raw.lens_sample[2] = 5;
        raw.lens_sample[3] = 1;
        const Camera c = parse_camera(raw);
        check("the lens sample (0.3, -0.4), index 5, marked, is copied", c.lens.x == 0.3f && c.lens.y == -0.4f && c.lens.index == 5 && c.lens.mark);
        raw.lens_sample[0] = NAN;
        raw.lens_sample[1] = 2;
        raw.lens_sample[2] = -3;
        raw.lens_sample[3] = NAN;
        const Camera bad = parse_camera(raw);
        check("a NaN coordinate is 0, 2 clamps to 1, index -3 is 0, and a NaN mark is unmarked", bad.lens.x == 0.0f && bad.lens.y == 1.0f
            && bad.lens.index == 0 && !bad.lens.mark);
        raw.lens_sample[0] = -5;
        raw.lens_sample[2] = 7.9f;
        raw.lens_sample[3] = 0;
        const Camera low = parse_camera(raw);
        check("-5 clamps to -1, index 7.9 is 7, and mark 0 is unmarked", low.lens.x == -1.0f && low.lens.index == 7 && !low.lens.mark);
    }
    {
        FrameReport report;
        report.armed = true;
        report.accumulate = AccumulateMode::add;
        report.accumulated = 12;
        check("an accumulating frame says so without a count", describe(report) == "found nothing; accumulating");
        report.accumulate = AccumulateMode::present;
        check("a presenting frame says how many samples", describe(report) == "found nothing; presenting 12 samples");
    }
    std::printf("%s: %d failed\n", __FILE__, failures);
    return failures == 0 ? 0 : 1;
}
