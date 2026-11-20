// The physical sky: the atmosphere's transmittance against its closed form, the sky's colors at noon, sunset and
// twilight, its light against the sun's, haze, and the sky map's scale to the game's light.
#include "../core/night_lamps.h"
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
    check("the sun at the zenith reaches the ground through the closed form's 0.840, 0.789 and 0.693",
        std::fabs(zenith[0] - 0.8401f) < 0.005f && std::fabs(zenith[1] - 0.7892f) < 0.005f && std::fabs(zenith[2] - 0.6934f) < 0.005f);
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

    const Vec3f integrated = sun_transmittance(ground_level, std::sin(10 * 3.14159265f / 180)), tabled = sky_transmittance(air, toward(10));
    check("the transmittance without the tables is the tables' within 1% at 10 degrees",
        std::fabs(integrated[0] - tabled[0]) < 0.01f * tabled[0] && std::fabs(integrated[2] - tabled[2]) < 0.01f * tabled[2]);
    check("and nothing below the horizon", luminance(sun_transmittance(ground_level, -0.05f)) == 0);

    // The twilight the atmosphere makes by itself.
    const Vec3f anti_low = sky_radiance(air, toward(1, 180), toward(-2)), anti_band = sky_radiance(air, toward(10, 180), toward(-2));
    std::printf("sun 2 degrees down, opposite it: at 1 degree (%.3g %.3g %.3g), at 10 degrees (%.3g %.3g %.3g)\n", anti_low[0], anti_low[1], anti_low[2],
        anti_band[0], anti_band[1], anti_band[2]);
    check("with the sun 2 degrees down the earth's shadow lies dark on the horizon opposite it, under a pinker belt of Venus",
        luminance(anti_band) > luminance(anti_low) && anti_band[0] / anti_band[2] > anti_low[0] / anti_low[2]);
    const Vec3f blue_hour = sky_radiance(air, { 0, 1, 0 }, toward(-6)), day_zenith = sky_radiance(air, { 0, 1, 0 }, toward(30));
    SkySettings no_ozone = ground_level;
    no_ozone.ozone = 0;
    const Vec3f gray_hour = sky_radiance(make_atmosphere(no_ozone), { 0, 1, 0 }, toward(-6));
    std::printf("zenith blue over red: sun 30 degrees up %.2f, 6 degrees down %.2f, without ozone %.2f\n", day_zenith[2] / day_zenith[0],
        blue_hour[2] / blue_hour[0], gray_hour[2] / gray_hour[0]);
    check("ozone keeps the zenith blue when the sun is 6 degrees down, which without it would turn gray",
        blue_hour[2] / blue_hour[0] > 1.5f && gray_hour[2] / gray_hour[0] < 0.75f * blue_hour[2] / blue_hour[0]);
    check("and the sky still glows a little when the sun is 18 degrees down", luminance(sky_radiance(air, toward(5, 0), toward(-18))) > 0);

    const float noon_floor = level_illuminance(air, { noon, 1 }), sunset_floor = level_illuminance(air, { toward(0), 1 });
    std::printf("on a level floor: the sun at 60 degrees %.3f, at the horizon %.5f of its light above the air\n", noon_floor, sunset_floor);
    check("the sun at 60 degrees lights a level floor with 0.75 to 0.9 of its light above the air, about 100,000 lux", noon_floor > 0.75f && noon_floor < 0.9f);
    check("at sunset with 0.2% to 1% of it, 300 to 1,300 lux", sunset_floor > 0.002f && sunset_floor < 0.01f);
    const float full = moon_illuminance(0);
    check("a full moon above the air is 2.49 millionths of the sun", std::fabs(full - 2.49e-6f) < 0.03e-6f);
    check("a quarter moon a tenth of that, a new moon nothing that shows", std::fabs(moon_illuminance(1.5708f) / full - 0.09f) < 0.02f
        && moon_illuminance(3.1416f) < 1e-3f * full);
    // The disc's light, summed over a grid on it, as sky_ps.hlsl shades it with a uniform albedo.
    const auto disc_light = [](float phase)
    {
        const float r = kMoonRadius, cell = 2.0f / 200;
        double sum = 0;
        for (int j = 0; j < 200; j++)
            for (int i = 0; i < 200; i++)
            {
                const float x = -1 + (i + 0.5f) * cell, y = -1 + (j + 0.5f) * cell;
                if (x * x + y * y >= 1)
                    continue;
                const float facing = std::sqrt(1 - x * x - y * y);
                // The sun at the phase angle off the viewer, in the x-z plane.
                const float lit = x * std::sin(phase) + facing * std::cos(phase);
                sum += lit > 0 ? lit / (lit + facing) * cell * cell * r * r : 0;
            }
        return float(sum) * moon_radiance(phase);
    };
    check("the moon's shaded disc sends moon_illuminance's light full and at a quarter",
        std::fabs(disc_light(0) / full - 1) < 0.02f && std::fabs(disc_light(1.5708f) / moon_illuminance(1.5708f) - 1) < 0.03f);
    check("earthshine is 1.7e-4 of the full moon's face at new moon and nothing at full",
        std::fabs(earthshine_radiance(3.14159265f) / (0.5f * moon_radiance(0)) - 1.7e-4f) < 1e-6f && earthshine_radiance(0) < 1e-12f);
    const float moon_floor = level_illuminance(air, { noon, full });
    check("a full moon 60 degrees up lights the floor 0.25 lux against the sun's 100,000", std::fabs(moon_floor / noon_floor - 2.5e-6f) < 0.5e-6f);

    const Vec3f ground_zenith = sky_transmittance(air, { 0, 1, 0 });
    SkyExposure exposure;
    exposure.day_light = 1.5f;
    const SkyLight no_moon = { { 0, -1, 0 }, full };
    const Vec3f day_scale = sky_scale(air, { { 0, 1, 0 }, 1 }, no_moon, exposure);
    check("by day the sun overhead lights a white surface white at the game's day light",
        std::fabs(day_scale[0] * ground_zenith[0] / 3.14159265f - 1.5f) < 0.01f && std::fabs(day_scale[2] * ground_zenith[2] / 3.14159265f - 1.5f) < 0.01f);
    const float morning = sky_scale(air, { toward(20), 1 }, no_moon, exposure)[1] / day_scale[1], afternoon_gain = sky_scale(air, { noon, 1 }, no_moon, exposure)[1] / day_scale[1];
    std::printf("INFO exposure over the overhead sun's: %.4f with the sun 60 degrees up, %.4f at 20\n", afternoon_gain, morning);
    check("a lower sun lifts the exposure by the square root of the fallen light, so a morning keeps its brightness: 1.08 at 60 degrees, 1.89 at 20",
        std::fabs(afternoon_gain - 1.08f) < 0.02f && std::fabs(morning - 1.89f) < 0.03f);
    const Vec3f dusk_scale = sky_scale(air, { toward(-6), 1 }, no_moon, exposure);
    check("past it the exposure follows the falling light", dusk_scale[1] > 20 * day_scale[1]);
    exposure.night_stops = 15;
    check("and at night it stops at night_stops", std::fabs(sky_scale(air, { toward(-40), 1 }, no_moon, exposure)[1] / day_scale[1] - std::exp2(15.0f)) < 1);
    check("glow lights the night sky toward the horizon more than overhead, and nothing below it",
        luminance(night_glow(air, toward(10))) > luminance(night_glow(air, { 0, 1, 0 })) && luminance(night_glow(air, toward(-5))) == 0);

    SkyRequest dusk;
    dusk.sun = { toward(-1.0f), 1 };
    dusk.moon = { toward(40), full };
    check("clouds 3 km up still take the sun a degree after it set on the ground, and the moon once it is 3 degrees down",
        cloud_light(air, dusk, 3).illuminance == 1 && (dusk.sun.toward = toward(-3.0f), cloud_light(air, dusk, 3).illuminance == full));

    SkyRequest a;
    a.sun = { noon, 1 };
    a.moon = no_moon;
    a.exposure.day_light = 1;
    a.haze = 0.1f;
    const auto map_start = std::chrono::steady_clock::now();
    const SkyMap sky = physical_sky_map(air, a);
    std::printf("physical_sky_map took %.0f ms\n", std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - map_start).count());
    check("every texel of the map is real sky", sky.real == kSkyMapSide * kSkyMapSide && !sky.fallback);
    // A white floor: the sky's light against the sun's at 60 degrees.
    const Vec3f k = sky_scale(air, a.sun, a.moon, a.exposure), t60 = sky_transmittance(air, noon);
    const float diffuse = luminance(sky_irradiance(sky, { 0, 1, 0 })), direct = luminance({ k[0] * t60[0], k[1] * t60[1], k[2] * t60[2] }) / 3.14159265f * std::sin(60 * 3.14159265f / 180);
    std::printf("diffuse over direct on a white floor at noon: %.3f\n", diffuse / direct);
    check("a clear sky's light on the floor is a sixth to a quarter of the sun's", diffuse / direct > 0.15f && diffuse / direct < 0.25f);
    SkyRequest brighter = a;
    brighter.exposure.day_light = 2;
    const SkyMap twice = physical_sky_map(air, brighter);
    check("the map follows the game's day light", std::fabs(twice.texels[4100] - 2 * sky.texels[4100]) < 1e-5f * std::fabs(sky.texels[4100]) + 1e-9f);
    SkyRequest moonlit = a;
    moonlit.sun = { toward(-40), 1 };
    moonlit.moon = { noon, full };
    const SkyMap night_map = physical_sky_map(air, moonlit);
    const Vec3f night_up = sky_irradiance(night_map, { 0, 1, 0 });
    check("a moonlit sky is blue, as a sunlit one", night_up[2] > night_up[0]);

    SkySettings hazy = ground_level;
    hazy.aerosol_depth = 0.4f;
    const Vec3f hazy_horizon = sky_radiance(make_atmosphere(hazy), toward(3, 90), noon);
    check("haze whitens the horizon", hazy_horizon[2] / hazy_horizon[0] < horizon[2] / horizon[0]);

    const Vec3f around = sky_horizon(sky);
    check("the horizon's color is the map's between it and 10 degrees up, paler than the zenith's",
        luminance(around) > luminance(sky_irradiance(sky, { 0, 1, 0 })) * 0.5f && around[2] / around[0] < up[2] * k[2] / (up[0] * k[0]));

    SkyRequest b = a;
    b.sun.toward = toward(59.9f);
    check("a tenth of a degree is no new sky", !sky_request_differs(a, b));
    b.sun.toward = toward(59.5f);
    check("half a degree is", sky_request_differs(a, b));
    SkyRequest c = a;
    c.moon.illuminance *= 1.05f;
    check("so is a moon 5% brighter", sky_request_differs(a, c));
    {
        SkyBuilder builder;
        check("a builder has no sky before its first request", builder.latest() == nullptr);
        builder.request(a);
        builder.request(b);
        std::shared_ptr<const SkyResult> got;
        for (int i = 0; i < 400 && !(got && got->request.sun.toward == b.sun.toward); i++)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            got = builder.latest();
        }
        SkySettings clear;
        clear.aerosol_depth = 0.1f;
        const Atmosphere built = make_atmosphere(clear);
        const SkyMap direct_map = physical_sky_map(built, b);
        check("it builds the newest request, as physical_sky_map does, and keeps its atmosphere and scale",
            got && got->request.sun.toward == b.sun.toward && got->map.texels == direct_map.texels && got->atmosphere
                && got->atmosphere->transmittance == built.transmittance && got->scale == sky_scale(built, b.sun, b.moon, b.exposure));
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

    EnhancementsCelestial body {};
    body.size = sizeof body;
    check("a celestial too short for its sun is none", (body.size = 8, !parse_celestial(&body)) && !parse_celestial(nullptr));
    body = { sizeof body, 1, { 0, 2, 0, 0 }, { 3, 0, 4, 9 }, { 0, 0, 1, 0 }, { 1, 0, 0, 0 }, { 0, 1, 0, 0 }, { 0, 0, 1, 0 }, -1, 5, 40, 12, 1, 1, 0 };
    const std::optional<Celestial> parsed = parse_celestial(&body);
    check("a celestial's directions come out unit length, its phase clamped to pi, a day light below 0 read as 1.5, its exposure clamped",
        parsed && parsed->stars && parsed->toward_sun == std::array<float, 3> { 0, 1, 0 } && parsed->toward_moon == std::array<float, 3> { 0.6f, 0, 0.8f }
            && std::fabs(parsed->moon_phase - 3.14159265f) < 1e-6f && parsed->day_light == 1.5f && parsed->twilight_power == 2 && parsed->night_stops == 30
            && parsed->days == 12);
    body.stars_y[1] = std::numeric_limits<float>::quiet_NaN();
    check("an axis that is no number leaves the moon and the stars unknown", !parse_celestial(&body)->stars);
    body.size = offsetof(EnhancementsCelestial, day_light);
    check("a caller without the exposure keeps its defaults", parse_celestial(&body)->day_light == 1.5f && parse_celestial(&body)->night_stops == 17);

    raw.size = sizeof raw;
    raw.clouds = { 1, 2.0f, 3000, 1, 5, 9, -1, 0.5f, 10, 45, 2, 0.5f, 200, 1 };
    const CloudSettings clouds = parse_settings(raw).clouds;
    check("the clouds read their block and clamp it: coverage to 1, thickness to 10 m, size to 20 m, stretch to 6, density to 0, steps to 64, streaks kept",
        clouds.enabled && clouds.coverage == 1 && clouds.base == 3000 && clouds.thickness == 10 && clouds.size == 20 && clouds.stretch == 6
            && clouds.density == 0 && clouds.detail == 0.5f && clouds.wind_speed == 10 && clouds.wind_direction == 45 && clouds.sun == 2
            && clouds.ambient == 0.5f && clouds.steps == 64 && clouds.shape == CloudShape::streaks && kPasses[11].id == PassId::clouds && kPasses[11].wanted(parse_settings(raw)));
    raw.clouds.shape = 2;
    check("shape 2 reads as a sheet", parse_settings(raw).clouds.shape == CloudShape::sheet);
    raw.cloud_pattern.pattern = ENHANCEMENTS_CLOUD_PATTERN_OVERCAST;
    check("pattern 3 reads as the painted overcast", parse_settings(raw).clouds.pattern == CloudPattern::overcast);
    raw.cloud_pattern.pattern = 9;
    check("an unknown pattern reads as our noise", parse_settings(raw).clouds.pattern == CloudPattern::noise);
    raw.cloud_pattern.pattern = ENHANCEMENTS_CLOUD_PATTERN_STREAKS;
    raw.size = offsetof(EnhancementsSettings, cloud_pattern);
    check("a caller from before the pattern gets our noise", parse_settings(raw).clouds.pattern == CloudPattern::noise);
    raw.size = sizeof raw;
    raw.cloud_pattern.pattern = 0;
    raw.clouds.shape = 7;
    check("an unknown shape reads as heaps", parse_settings(raw).clouds.shape == CloudShape::heaps);
    raw.clouds.shape = 1;
    raw.size = offsetof(EnhancementsSettings, clouds) + offsetof(EnhancementsClouds, shape);
    check("a caller built before the shape draws heaps", parse_settings(raw).clouds.enabled && parse_settings(raw).clouds.shape == CloudShape::heaps);
    raw.size = offsetof(EnhancementsSettings, clouds);
    check("a caller built before the clouds draws none", !parse_settings(raw).clouds.enabled && parse_settings(raw).clouds.coverage == 0.5f);
    {
        EnhancementsSettings wet {};
        wet.size = sizeof wet;
        wet.enabled = 1;
        wet.rain.amount = 0.6f;
        const Settings rainy = parse_settings(wet);
        wet.rain.amount = 7;
        const float clamped = parse_settings(wet).rain;
        wet.rain.amount = NAN;
        const float unknown = parse_settings(wet).rain;
        wet.size = offsetof(EnhancementsSettings, rain);
        check("rain reads its amount, clamps it to 1 and a NaN to none, a caller before it has none, and only rain wants its pass at bloom",
            rainy.rain == 0.6f && clamped == 1 && unknown == 0 && parse_settings(wet).rain == 0 && wanted_steps(rainy).has(Step::bloom));
        EnhancementsSettings storm = wet;
        storm.size = sizeof storm;
        storm.rain = { 0, 0.8f };
        const Settings lit = parse_settings(storm);
        storm.size = offsetof(EnhancementsSettings, rain) + sizeof(float);
        check("lightning reads its rate and wants the rain pass without rain; a version 40 caller has none",
            lit.lightning == 0.8f && lit.rain == 0 && wanted_steps(lit).has(Step::bloom) && parse_settings(storm).lightning == 0);
        EnhancementsSettings banks = storm;
        banks.size = sizeof banks;
        banks.mist = { 0.7f, 2000 };
        const Settings banked = parse_settings(banks);
        banks.size = offsetof(EnhancementsSettings, mist);
        check("the mist reads its patchiness and clamps its bank size to 500 m; a version 41 caller has an even haze 40 m banks would make",
            banked.atmosphere.patchiness == 0.7f && banked.atmosphere.patch_size == 500 && parse_settings(banks).atmosphere.patchiness == 0
            && parse_settings(banks).atmosphere.patch_size == 40);
        EnhancementsSettings lifted = banks;
        lifted.size = sizeof lifted;
        lifted.atmosphere.enabled = 0;
        lifted.scene_light = { 9 };
        const Settings lit_scene = parse_settings(lifted);
        lifted.size = offsetof(EnhancementsSettings, scene_light);
        check("the scene's light reads its stops, clamped to 3, and wants the tone map's reader; a version 42 caller has none",
            lit_scene.scene_light == 3 && wanted_steps(lit_scene).has(Step::tonemap_read) && parse_settings(lifted).scene_light == 0);
        EnhancementsSettings through = lifted;
        through.size = sizeof through;
        through.transmission = { 5 };
        const Settings leaves = parse_settings(through);
        through.size = offsetof(EnhancementsSettings, transmission);
        check("light through leaves reads its strength, clamped to 2, and wants the combine step and bloom; a version 43 caller has none",
            leaves.transmission == 2 && wanted_steps(leaves).has(Step::combine) && wanted_steps(leaves).has(Step::bloom) && parse_settings(through).transmission == 0);
        int strikes = 0, bolts = 0;
        float brightest = 0, struck = -1, bolted = -1;
        bool dark_without = true, repeats = true;
        for (int i = 0; i < 700 * 50; i++)
        {
            const double t = 1000 + i / 50.0;
            const Lightning l = lightning_at(t, 1), again = lightning_at(t, 1);
            if (l.flash > 0.3f && l.azimuth != struck)
                strikes++, struck = l.azimuth;
            if (l.bolt > 0 && l.azimuth != bolted)
                bolts++, bolted = l.azimuth;
            brightest = std::max(brightest, l.flash);
            dark_without = dark_without && lightning_at(t, 0).flash == 0;
            repeats = repeats && l.flash == again.flash && l.azimuth == again.azimuth;
        }
        check("lightning at rate 1 strikes about every 7 seconds over 700, most with a bolt, never past 1.5, none at rate 0, and the same at the same time",
            strikes > 70 && strikes < 140 && bolts > 40 && brightest > 0.8f && brightest <= 1.5f && dark_without && repeats);
    }
    {
        const std::optional<ShelterMove> first = shelter_move(std::nullopt, { 10, 5, -10 });
        check("the shelter map's first frame clears it, its corner 32 meters below the camera's nearest 16 on x and z",
            first && first->clear && first->place.origin[0] == -16 && first->place.origin[1] == -48 && first->shift[0] == 0 && first->shift[1] == 0);
        const std::optional<ShelterMove> still = shelter_move(first->place, { 11, 5, -10.5 });
        check("a meter's move keeps the map where it is", still && !still->clear && still->place.origin[0] == -16 && still->shift[0] == 0 && still->shift[1] == 0);
        const std::optional<ShelterMove> on = shelter_move(still->place, { 26, 5, -10.5 });
        check("past the halfway to the next 16 meters on x, the corner moves 16 meters and the contents 64 cells",
            on && !on->clear && on->place.origin[0] == 0 && on->place.origin[1] == -48 && on->shift[0] == 64 && on->shift[1] == 0);
        const std::optional<ShelterMove> back = shelter_move(on->place, { 26, 5, -30 });
        check("back along z by 16 meters, the contents shift -64 cells", back && !back->clear && back->shift[0] == 0 && back->shift[1] == -64);
        const std::optional<ShelterMove> jump = shelter_move(back->place, { 26, 40, -30 });
        check("a 35 meter jump in one frame clears the map, with no shift", jump && jump->clear && jump->shift[0] == 0 && jump->shift[1] == 0);
        check("a camera that is not finite has no map", !shelter_move(std::nullopt, { NAN, 0, 0 }));
    }
    {
        // The world's light at the drawn sky's exposure, for a day light of 1.5 and the default exposure.
        const Atmosphere clear = make_atmosphere(SkySettings {});
        const auto lit = [&clear](float sun, float moon, float phase) {
            SkyRequest r;
            r.sun = { toward(sun), 1 };
            r.moon = { toward(moon), moon_illuminance(phase) };
            r.exposure = { 1.5f, 0.85f, 17 };
            return world_light(clear, r);
        };
        const auto near = [](float a, float b) { return std::fabs(a - b) <= 0.01f * std::fabs(b); };
        const WorldLight overhead = lit(90, -90, 3.14f);
        check("with the sun overhead its light on a white surface is the day light, about 5200 K warm, and the sky's share is 1",
            std::fabs(luminance(overhead.sun) - 1.5f) < 2e-3f && std::fabs(overhead.sun[0] - 1.5f * 1.066f) < 2e-3f && std::fabs(overhead.sun[2] - 1.5f * 0.895f) < 2e-3f
            && std::fabs(overhead.sky_share - 1) < 1e-5f);
        const WorldLight afternoon = lit(30, -90, 3.14f);
        check("with the sun 30 degrees up its light is 1.811, lifted by the morning's exposure, and warmer, the sky's share 1.082",
            near(luminance(afternoon.sun), 1.8107f) && afternoon.sun[0] > afternoon.sun[2] && near(afternoon.sky_share, 1.08161f));
        const WorldLight blue_hour = lit(-6, -90, 3.14f);
        check("with the sun 6 degrees down there is no sunlight and the exposed sky keeps a quarter of a day's, 0.247",
            luminance(blue_hour.sun) == 0 && near(blue_hour.sky_share, 0.24674f));
        const WorldLight full = lit(-30, 40, 0);
        check("a full moon 40 degrees up lights a white surface 0.909 and the sky's share is 0.189: the night is exposed by its light on a surface facing it, 17% under its light on the ground",
            near(luminance(full.moon), 0.90893f) && near(full.sky_share, 0.18912f));
        const WorldLight rising = lit(-30, 4, 0), high = lit(-30, 60, 0);
        check("a full moon 4 degrees up lights what faces it less than one 60 degrees up, through more air: 0.143 against 0.894",
            near(luminance(rising.moon), 0.14262f) && near(luminance(high.moon), 0.89372f));
        const WorldLight moonless = lit(-30, -30, 0);
        check("with no moon only the night's glow is left, the sky's share 0.00323, held by the 17 stops' limit", luminance(moonless.moon) == 0 && near(moonless.sky_share, 0.0032304f));
        check("auto exposure works fully till the sun is 6 degrees down, half at 9 and not at all from 12",
            auto_exposure_share({ toward(20), 1 }) == 1 && auto_exposure_share({ toward(-6), 1 }) == 1
            && near(auto_exposure_share({ toward(-9), 1 }), 0.5f) && auto_exposure_share({ toward(-12), 1 }) == 0 && auto_exposure_share({ toward(-40), 1 }) == 0);
    }
    {
        EnhancementsCelestial raw {};
        raw.size = sizeof raw;
        raw.toward_sun[1] = 1;
        raw.ambient_scale = 0.25f;
        check("the celestial's ambient scale reads as given", parse_celestial(&raw)->ambient_scale == 0.25f);
        raw.ambient_scale = 9;
        check("and clamps to 4", parse_celestial(&raw)->ambient_scale == 4);
        raw.size = offsetof(EnhancementsCelestial, ambient_scale);
        check("a caller built before it keeps the ambient as it is", parse_celestial(&raw)->ambient_scale == 1);
    }
    {
        // Two lamps before a camera at the origin looking down -z (Unity's view space flips z): the near one's reach comes
        // nearer, so it goes first, in view space.
        Camera camera {};
        const float identity[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, -1, 0, 0, 0, 0, 1 };
        std::copy(identity, identity + 16, camera.world_to_view);
        EnhancementsShotLight far_lamp {}, near_lamp {};
        far_lamp.position[2] = 40; far_lamp.range = 10; far_lamp.color[0] = far_lamp.color[1] = far_lamp.color[2] = 1; far_lamp.cos_outer = -2; far_lamp.cos_inner = -2;
        near_lamp = far_lamp;
        near_lamp.position[0] = 3; near_lamp.position[2] = 8;
        const NightLamps lamps = night_lamps({ far_lamp, near_lamp }, camera);
        check("the lamps nearest the camera go first, in view space, with their range and cone", lamps.info[0] == 2 && lamps.rows[0][0][0] == 3
            && lamps.rows[0][0][2] == -8 && lamps.rows[1][0][2] == -40 && lamps.rows[0][0][3] == 10 && lamps.rows[0][1][3] == -2);
        check("no lamps are none", night_lamps({}, camera).info[0] == 0);
    }
    return failures == 0 ? 0 : 1;
}
