// The physical sky: Earth's atmosphere as Hillaire's "A Scalable and Production Ready Sky and Atmosphere Rendering
// Technique" (EGSR 2020) models it, Rayleigh and Mie scattering and ozone absorption over a spherical planet, with his
// multiple-scattering approximation. Computed on the CPU into the tracer's sky map, so the traced shot's sky light, the
// ambient pass and the haze take one sky whose brightness against the sun is physical. Directions are the world's, y up.
#pragma once

#include "light.h"

#include <vector>

namespace enhancements
{
    struct SkySettings
    {
        float aerosol_depth = 0.1f; // the dust and droplets' optical depth straight up: 0.1 a clear day, 0.3 hazy and whiter
        float ground_albedo = 0.3f; // the planet's ground below the horizon
        float altitude_km = 0.2f;   // the viewer's height above the ground
    };

    // The atmosphere's lookup tables for a setting: the transmittance to the atmosphere's top, and the multiple
    // scattering's contribution per unit scattering coefficient.
    struct Atmosphere
    {
        SkySettings settings;
        std::vector<Vec3f> transmittance;   // kTransmittanceWidth x kTransmittanceHeight, over Bruneton's (mu, r) mapping
        std::vector<Vec3f> multiple;        // kMultipleSide x kMultipleSide, over the light's cosine and the height
    };
    constexpr uint32_t kTransmittanceWidth = 256, kTransmittanceHeight = 64, kMultipleSide = 32;

    Atmosphere make_atmosphere(const SkySettings &settings);

    // The light's transmittance through the atmosphere to the viewer, per channel: what of a light above the
    // atmosphere reaches it along toward_light (unit, y up); 0 once the planet hides the light.
    Vec3f sky_transmittance(const Atmosphere &atmosphere, const Vec3f &toward_light);

    // The sky's radiance seen along view (unit, y up), for a light of unit illuminance above the atmosphere along
    // toward_light, the light's own disc left out. Below the horizon it is the lit ground seen through the air.
    Vec3f sky_radiance(const Atmosphere &atmosphere, const Vec3f &view, const Vec3f &toward_light);

    // The physical sky as the tracer's sky map, at the game's brightness: scaled so the light's direct light, through the
    // atmosphere at its elevation but at least min_elevation radians, shows on a white surface facing it what the game's
    // light shows, light_shade's luminance. Its color stays the model's. Every texel is real.
    SkyMap physical_sky_map(const Atmosphere &atmosphere, const Vec3f &toward_light, const Vec3f &light_shade, float min_elevation = 0.1f);
}
