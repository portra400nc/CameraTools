// The physical sky: Earth's atmosphere as Hillaire's "A Scalable and Production Ready Sky and Atmosphere Rendering
// Technique" (EGSR 2020) models it, Rayleigh and Mie scattering and ozone absorption over a spherical planet, with his
// multiple-scattering approximation. Computed on the CPU into the tracer's sky map, so the traced shot's sky light, the
// ambient pass and the haze take one sky whose brightness against the sun is physical. Directions are the world's, y up.
#pragma once

#include "light.h"

#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
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

    // The mean of the map's texels from the horizon to 10 degrees above it, all around: the light distant haze takes.
    Vec3f sky_horizon(const SkyMap &sky);

    // The colors' change while the sky's sun is below the horizon, when the main light is the moon: none by day, and
    // toward a blue of the same luminance, fully once the sun is 6 degrees down. sky_sun_y is the sky's sun direction's y.
    Vec3f night_tint(float sky_sun_y);

    // Multiplies the map's texels and harmonics by tint.
    void tint_sky(SkyMap &sky, const Vec3f &tint);

    // What a physical sky is built for.
    struct SkyRequest
    {
        Vec3f toward_light {};      // the main light's direction, unit
        Vec3f shade {};             // what a white surface facing it shows, as physical_sky_map scales to
        float haze = 0.1f;          // SkySettings::aerosol_depth
        Vec3f tint = { 1, 1, 1 };   // night_tint's
    };

    // Whether two requests build skies far enough apart to build again: the light's direction a quarter of a degree, its
    // luminance 1%, the haze at all, or the tint 0.005 in a channel.
    bool sky_request_differs(const SkyRequest &a, const SkyRequest &b);

    struct SkyResult
    {
        SkyRequest request;
        SkyMap map;                 // tinted
        Vec3f horizon {};           // sky_horizon of the map
        uint64_t version = 0;       // 1 for the builder's first sky, one more for each after it
    };

    // Builds physical skies on a thread of its own, so a frame never waits the few hundred milliseconds one takes. The
    // newest request wins: one asked for while another builds is built next, and any asked for before it are dropped. The
    // atmosphere's tables are kept while the haze stays.
    class SkyBuilder
    {
    public:
        SkyBuilder();
        ~SkyBuilder();
        SkyBuilder(const SkyBuilder &) = delete;
        SkyBuilder &operator=(const SkyBuilder &) = delete;

        void request(const SkyRequest &request);
        // The newest sky built, or null before the first.
        std::shared_ptr<const SkyResult> latest() const;

    private:
        void run();

        mutable std::mutex mutex_;
        std::condition_variable wake_;
        std::optional<SkyRequest> pending_;
        std::shared_ptr<const SkyResult> latest_;
        bool stop_ = false;
        std::thread thread_;
    };
}
