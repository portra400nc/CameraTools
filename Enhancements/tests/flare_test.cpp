// The lens flare: where the sun falls on the picture, the settings block, the visibility's ease, and shaders/flare.hlsli
// compiled as C++: no light without visibility or far off the picture, light around the source, and a screen blend that
// never passes white.
#include "../core/flare.h"
#include "../enhancements.h"

#include <cmath>
#include <cstdio>
#include <cstring>
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

    // A camera at the origin looking down world -Z, Unity's GL.GetGPUProjectionMatrix with P00 a and P11 b, reversed Z,
    // column-major, with the sun toward direction.
    Camera camera_toward(float x, float y, float z, float a = 1.25f, float b = 2.0f)
    {
        Camera c {};
        const float n = 0.1f, f = 1000;
        c.view_to_clip[0] = a;
        c.view_to_clip[5] = b;
        c.view_to_clip[10] = n / (f - n);
        c.view_to_clip[11] = -1;
        c.view_to_clip[14] = n * f / (f - n);
        for (int i = 0; i < 4; i++)
            c.world_to_view[i * 5] = 1;
        const float l = std::sqrt(x * x + y * y + z * z);
        c.toward_sun[0] = x / l;
        c.toward_sun[1] = y / l;
        c.toward_sun[2] = z / l;
        return c;
    }

    float brightest(const FlareConstants &constants, float visibility)
    {
        float most = 0;
        for (uint32_t y = 0; y < 800; y += 8)
            for (uint32_t x = 0; x < 1280; x += 8)
            {
                const std::array<float, 3> l = flare_light_at(x + 0.5f, y + 0.5f, constants, visibility);
                most = std::fmax(most, std::fmax(l[0], std::fmax(l[1], l[2])));
            }
        return most;
    }
}

int main(int, char **)
{
    const std::optional<FlareSun> ahead = flare_sun(camera_toward(0, 0, -1));
    check("a sun straight ahead falls on the picture's center, in front", ahead && ahead->in_front && near(ahead->x, 0) && near(ahead->y, 0));
    // 0.25 up per unit ahead through P11 2 is clip y 0.5, a quarter of the height above the center; 0.2 right through P00
    // 1.25 is clip x 0.25, at aspect 1.6 a fifth of the height right of it.
    const std::optional<FlareSun> up_right = flare_sun(camera_toward(0.2f, 0.25f, -1));
    check("a sun up and to the right falls a quarter of the height above the center and a fifth of it right",
        up_right && up_right->in_front && near(up_right->x, 0.2f) && near(up_right->y, -0.25f));
    const std::optional<FlareSun> behind = flare_sun(camera_toward(0.1f, 0.2f, 1));
    check("a sun behind the camera is not in front", behind && !behind->in_front);
    check("no sun direction gives no sun", !flare_sun(Camera {}));
    const FlareSun corner = flare_point(1, 0, 1.6f);
    check("the manual source at the top right corner lies 0.8 right and 0.5 up", near(corner.x, 0.8f) && near(corner.y, -0.5f));

    FlareSettings settings;
    settings.enabled = true;
    settings.strength = 2;
    const FlareConstants hidden = flare_constants(settings, *behind, { 1280, 800 }, true);
    check("a sun behind the camera gives the flare no strength", hidden.sun[2] == 0);
    const FlareConstants shown = flare_constants(settings, FlareSun { 0.3f, -0.3f, true }, { 1280, 800 }, true);
    check("a sun in front gives it the setting's strength and the picture's aspect", shown.sun[2] == 2 && near(shown.sun[3], 1.6f));
    check("the flare lights the picture around a sun on it", brightest(shown, 1) > 0.05f);
    check("without visibility the flare draws nothing", brightest(shown, 0) == 0);
    const FlareConstants far_off = flare_constants(settings, FlareSun { 5, -3, true }, { 1280, 800 }, true);
    check("a sun several heights off the picture draws nothing", brightest(far_off, 1) == 0);

    std::vector<float> white(size_t(1280) * 800 * 3, 1.0f);
    const std::vector<float> over_white = render_flare(white, shown, 1);
    bool stays = true;
    for (const float v : over_white)
        stays = stays && v == 1.0f;
    check("screened over white, the flare leaves white", stays);
    std::vector<float> black(size_t(1280) * 800 * 3, 0.0f);
    const std::vector<float> over_black = render_flare(black, shown, 1);
    bool within = true;
    for (const float v : over_black)
        within = within && v >= 0 && v <= 1;
    check("screened over black, every channel stays from 0 to 1", within);

    check("the visibility jumps at 0 seconds", flare_ease(0, 1.0f / 60) == 1);
    check("over the setting's seconds it covers 99% of the way", near(flare_ease(0.5f, 0.5f), 0.99f));

    EnhancementsSettings raw {};
    raw.size = sizeof raw;
    raw.lens_flare.enabled = 1;
    raw.lens_flare.source = ENHANCEMENTS_FLARE_SOURCE_MANUAL;
    raw.lens_flare.position[0] = 0.25f;
    raw.lens_flare.position[1] = 9;
    raw.lens_flare.strength = std::numeric_limits<float>::quiet_NaN();
    raw.lens_flare.size = 10;
    const Settings parsed = parse_settings(raw);
    check("the block reads the manual source and its position, clamped to 3 below the picture",
        parsed.flare.enabled && parsed.flare.source == FlareSource::manual && parsed.flare.position[0] == 0.25f && parsed.flare.position[1] == 3);
    check("a strength that is not a number takes the default, and the size clamps to 4", parsed.flare.strength == 1 && parsed.flare.size == 4);
    raw.size = offsetof(EnhancementsSettings, lens_flare);
    check("a caller built before the block gets the flare off", !parse_settings(raw).flare.enabled);
    raw.size = sizeof raw;
    raw.lens_flare.source = 7;
    check("an unknown source reads as the sun", parse_settings(raw).flare.source == FlareSource::sun);
    return failures == 0 ? 0 : 1;
}
