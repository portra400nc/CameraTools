// Auto exposure: shaders/exposure.hlsli compiled as C++, and the settings block that turns it on.
#include "../core/exposure.h"
#include "../core/plan.h"
#include "../enhancements.h"

#include <cmath>
#include <cstddef>
#include <cstdio>
#include <limits>

using namespace enhancements;

namespace
{
    int failures = 0;

    void check(const char *name, bool ok)
    {
        std::printf("%s %s\n", ok ? "PASS" : "FAIL", name);
        failures += ok ? 0 : 1;
    }

    bool near(float a, float b, float tolerance = 1e-4f) { return std::fabs(a - b) <= tolerance; }
}

int main()
{
    check("a white pixel meters at 0 stops", near(exposure_log_luminance(1, 1, 1), 0));
    check("a black pixel is floored at 1e-4", near(exposure_log_luminance(0, 0, 0), -13.2877f));
    check("green weighs most, as luminance does", near(exposure_log_luminance(0, 1, 0), std::log2(0.7152f)));

    // The snowy town capture of 2026-11-03: a log mean of 0.0354 under the game's exposure of 0.985.
    const float snow = std::log2(0.0354f);
    check("strength 0 keeps the game's exposure", exposure_target_ev(snow, 0.985f, 0) == 0);
    check("strength 1 brings the snowy town to middle gray, 2.37 stops up", near(exposure_target_ev(snow, 0.985f, 1), 2.3678f, 1e-3f));
    check("half strength goes half the way", near(exposure_target_ev(snow, 0.985f, 0.5f), 1.1839f, 1e-3f));
    check("a scene already at middle gray stays", near(exposure_target_ev(std::log2(0.18f / 0.985f), 0.985f, 1), 0));
    check("a night scene is raised 3 stops at most", exposure_target_ev(-12, 0.985f, 1) == 3);
    check("a bright scene is lowered 2 stops at most", exposure_target_ev(4, 0.985f, 1) == -2);

    check("one frame moves the ease's share of the way", near(exposure_next_ev(0, 2, 0.25f), 0.5f));
    check("an ease of 1 jumps", exposure_next_ev(-1, 2, 1) == 2);
    const float nan = std::numeric_limits<float>::quiet_NaN();
    check("a last frame of no value jumps to the target", exposure_next_ev(nan, 1.5f, 0.1f) == 1.5f);
    check("as does one out of range", exposure_next_ev(9, 1.5f, 0.1f) == 1.5f);

    EnhancementsSettings raw {};
    raw.size = sizeof raw;
    raw.auto_exposure.strength = 0.6f;
    check("the block's strength reaches the tonemap settings", parse_settings(raw).tonemap.auto_exposure == 0.6f);
    raw.auto_exposure.strength = 3;
    check("and clamps to 1", parse_settings(raw).tonemap.auto_exposure == 1);
    raw.auto_exposure.strength = nan;
    check("not finite reads as off", parse_settings(raw).tonemap.auto_exposure == 0);
    raw.auto_exposure.strength = 0.6f;
    raw.size = offsetof(EnhancementsSettings, auto_exposure);
    check("a caller built before the block gets it off", parse_settings(raw).tonemap.auto_exposure == 0);
    return failures == 0 ? 0 : 1;
}
