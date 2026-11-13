// The physical sky: the atmosphere's transmittance against its closed form, the sky's colors at noon, sunset and
// twilight, its light against the sun's, haze, and the sky map's scale to the game's light.
#include "../core/plan.h"
#include "../core/sky.h"
#include "../enhancements.h"

#include <cstddef>
#include <limits>
#include <thread>

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

    const Vec3f around = sky_horizon(sky);
    check("the horizon's color is the map's between it and 10 degrees up, paler than the zenith's",
        luminance(around) > luminance(sky_irradiance(sky, { 0, 1, 0 })) * 0.5f && around[2] / around[0] < up[2] / up[0]);
    check("by day the night tint changes nothing", night_tint(0.2f) == Vec3f { 1, 1, 1 });
    const Vec3f night = night_tint(-0.2f);
    check("past 6 degrees down it is a blue of the same luminance", night[2] > night[0] && std::fabs(luminance(night) - 1) < 0.01f);
    SkyMap tinted = sky;
    tint_sky(tinted, { 2, 1, 1 });
    check("a tint scales the texels and the harmonics", tinted.texels[4100 - 4100 % 4] == 2 * sky.texels[4100 - 4100 % 4]
        && tinted.harmonics[0][0] == 2 * sky.harmonics[0][0] && tinted.harmonics[0][1] == sky.harmonics[0][1]);

    SkyRequest a;
    a.toward_light = noon;
    a.shade = { 1, 1, 1 };
    a.haze = 0.1f;
    SkyRequest b = a;
    b.toward_light = toward(59.9f);
    check("a tenth of a degree is no new sky", !sky_request_differs(a, b));
    b.toward_light = toward(59.5f);
    check("half a degree is", sky_request_differs(a, b));
    {
        SkyBuilder builder;
        check("a builder has no sky before its first request", builder.latest() == nullptr);
        builder.request(a);
        builder.request(b);
        std::shared_ptr<const SkyResult> got;
        for (int i = 0; i < 400 && !(got && got->request.toward_light == b.toward_light); i++)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            got = builder.latest();
        }
        SkySettings clear;
        clear.aerosol_depth = 0.1f;
        const SkyMap direct = physical_sky_map(make_atmosphere(clear), b.toward_light, b.shade);
        check("it builds the newest request, as physical_sky_map does", got && got->request.toward_light == b.toward_light && got->map.texels == direct.texels);
    }

    EnhancementsSettings raw {};
    raw.size = sizeof raw;
    check("the physical sky is off by default", !parse_settings(raw).sky.enabled);
    raw.sky = { 1, 0.25f };
    const Settings on = parse_settings(raw);
    check("switched on it keeps its haze, and a traced shot gathers its map", on.sky.enabled && on.sky.haze == 0.25f && on.gather.sky == GatherSky::map);
    raw.sky.haze = 3;
    check("the haze clamps to 0.5", parse_settings(raw).sky.haze == 0.5f);
    raw.sky.haze = std::numeric_limits<float>::quiet_NaN();
    check("and a haze that is no number reads as 0.1", parse_settings(raw).sky.haze == 0.1f);
    raw.size = offsetof(EnhancementsSettings, sky);
    check("a caller built before the block keeps the game's sky", !parse_settings(raw).sky.enabled && parse_settings(raw).gather.sky != GatherSky::map);

    raw.size = sizeof raw;
    raw.clouds = { 1, 2.0f, 3000, 1, 5, 9, -1, 0.5f, 10, 45, 2, 0.5f, 200, 1 };
    const CloudSettings clouds = parse_settings(raw).clouds;
    check("the clouds read their block and clamp it: coverage to 1, thickness to 10 m, size to 20 m, stretch to 6, density to 0, steps to 64, streaks kept",
        clouds.enabled && clouds.coverage == 1 && clouds.base == 3000 && clouds.thickness == 10 && clouds.size == 20 && clouds.stretch == 6
            && clouds.density == 0 && clouds.detail == 0.5f && clouds.wind_speed == 10 && clouds.wind_direction == 45 && clouds.sun == 2
            && clouds.ambient == 0.5f && clouds.steps == 64 && clouds.shape == CloudShape::streaks && kPasses[7].id == PassId::clouds && kPasses[7].wanted(parse_settings(raw)));
    raw.clouds.shape = 2;
    check("shape 2 reads as a sheet", parse_settings(raw).clouds.shape == CloudShape::sheet);
    raw.clouds.shape = 7;
    check("an unknown shape reads as heaps", parse_settings(raw).clouds.shape == CloudShape::heaps);
    raw.clouds.shape = 1;
    raw.size = offsetof(EnhancementsSettings, clouds) + offsetof(EnhancementsClouds, shape);
    check("a caller built before the shape draws heaps", parse_settings(raw).clouds.enabled && parse_settings(raw).clouds.shape == CloudShape::heaps);
    raw.size = offsetof(EnhancementsSettings, clouds);
    check("a caller built before the clouds draws none", !parse_settings(raw).clouds.enabled && parse_settings(raw).clouds.coverage == 0.5f);
    return failures == 0 ? 0 : 1;
}
