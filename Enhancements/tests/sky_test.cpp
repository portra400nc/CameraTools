// The physical sky: the atmosphere's transmittance against its closed form, the sky's colors at noon, sunset and
// twilight, its light against the sun's, haze, and the sky map's scale to the game's light.
#include "../core/sky.h"

#include <chrono>
#include <cmath>
#include <cstdio>

using namespace enhancements;

namespace
{
    int failures = 0;

    void check(const char *name, bool ok)
    {
        std::printf("%s %s\n", ok ? "PASS" : "FAIL", name);
        failures += ok ? 0 : 1;
    }

    float luminance(const Vec3f &c) { return 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2]; }

    // Toward the light at elevation degrees, in the x-y plane.
    Vec3f toward(float degrees, float azimuth_degrees = 0)
    {
        const float e = degrees * 3.14159265f / 180, a = azimuth_degrees * 3.14159265f / 180;
        return { std::cos(e) * std::cos(a), std::sin(e), std::cos(e) * std::sin(a) };
    }
}

int main()
{
    const auto start = std::chrono::steady_clock::now();
    SkySettings ground_level;
    ground_level.altitude_km = 0.001f;
    const Atmosphere air = make_atmosphere(ground_level);
    std::printf("make_atmosphere took %.0f ms\n", std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());

    // exp(-(8 km of Rayleigh, the aerosols' 0.1, 15 km of ozone)) straight up from the ground.
    const Vec3f zenith = sky_transmittance(air, { 0, 1, 0 });
    check("the sun at the zenith reaches the ground through the closed form's 0.855, 0.789 and 0.693",
        std::fabs(zenith[0] - 0.8554f) < 0.005f && std::fabs(zenith[1] - 0.7892f) < 0.005f && std::fabs(zenith[2] - 0.6934f) < 0.005f);
    const Vec3f low = sky_transmittance(air, toward(2));
    check("a low sun is dimmer and redder", luminance(low) < 0.5f * luminance(zenith) && low[0] / low[2] > 2 * zenith[0] / zenith[2]);
    check("the planet hides a sun below the horizon", luminance(sky_transmittance(air, toward(-2))) == 0);

    const Vec3f noon = toward(60);
    const Vec3f up = sky_radiance(air, { 0, 1, 0 }, noon);
    check("the sky at noon is blue overhead", up[2] > up[1] && up[1] > up[0]);
    const Vec3f horizon = sky_radiance(air, toward(3, 90), noon);
    check("and brighter toward the horizon", luminance(horizon) > luminance(up));
    check("and brightest around the sun", luminance(sky_radiance(air, toward(55), noon)) > 3 * luminance(horizon));

    const Vec3f sunset = toward(1);
    const Vec3f glow = sky_radiance(air, toward(3), sunset);
    check("at sunset the horizon under the sun is red", glow[0] > glow[2]);
    check("at twilight, 10 degrees below, the zenith is under a hundredth of noon's",
        luminance(sky_radiance(air, { 0, 1, 0 }, toward(-10))) < 0.01f * luminance(up));
    const Vec3f below = sky_radiance(air, toward(-30), noon);
    check("below the horizon the lit ground shows", luminance(below) > 0 && luminance(below) < luminance(horizon));

    const auto map_start = std::chrono::steady_clock::now();
    const SkyMap sky = physical_sky_map(air, noon, { 1, 1, 1 });
    std::printf("physical_sky_map took %.0f ms\n", std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - map_start).count());
    check("every texel of the map is real sky", sky.real == kSkyMapSide * kSkyMapSide && !sky.fallback);
    // A white floor: the sky's light against the sun's at 60 degrees, which shows 1 x sin 60 on it.
    const float diffuse = luminance(sky_irradiance(sky, { 0, 1, 0 })), direct = std::sin(60 * 3.14159265f / 180);
    std::printf("diffuse over direct on a white floor at noon: %.3f\n", diffuse / direct);
    check("a clear sky's light on the floor is a sixth to a quarter of the sun's", diffuse / direct > 0.15f && diffuse / direct < 0.25f);
    const SkyMap twice = physical_sky_map(air, noon, { 2, 2, 2 });
    check("the map follows the game's light", std::fabs(twice.texels[4100] - 2 * sky.texels[4100]) < 1e-6f * std::fabs(sky.texels[4100]) + 1e-9f);

    SkySettings hazy = ground_level;
    hazy.aerosol_depth = 0.4f;
    const Vec3f hazy_horizon = sky_radiance(make_atmosphere(hazy), toward(3, 90), noon);
    check("haze whitens the horizon", hazy_horizon[2] / hazy_horizon[0] < horizon[2] / horizon[0]);
    return failures == 0 ? 0 : 1;
}
