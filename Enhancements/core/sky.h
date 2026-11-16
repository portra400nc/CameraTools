// The physical sky: Earth's atmosphere as Hillaire's "A Scalable and Production Ready Sky and Atmosphere Rendering
// Technique" (EGSR 2020) models it, Rayleigh and Mie scattering and ozone absorption over a spherical planet, with his
// multiple-scattering approximation. Computed on the CPU into the tracer's sky map, so the traced shot's sky light, the
// ambient pass and the haze take one sky whose brightness against the sun is physical, and into the tables the drawn sky
// reads. The sun and the moon light it, and the night adds its own glow. Directions are the world's, y up.
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
        float ozone = 1;            // multiplies the ozone layer's absorption: 1 the earth's, which keeps the twilight sky blue
        float sunset = 0;           // turns light the air reddened toward pink, 0 not at all to 1: the stylized sunset
    };

    // The atmosphere's lookup tables for a setting: the transmittance to the atmosphere's top, and the multiple
    // scattering's contribution per unit scattering coefficient.
    struct Atmosphere
    {
        SkySettings settings;
        std::vector<Vec3f> transmittance;   // kTransmittanceWidth x kTransmittanceHeight, over Bruneton's (mu, r) mapping
        std::vector<Vec3f> multiple;        // kMultipleSide x kMultipleSide, over the light's cosine and the height
        std::vector<float> irradiance;      // kIrradianceSize: the luminance the sky alone sheds on a level floor for a light
                                            // of unit illuminance, over the light's sine from kIrradianceLow to 1
    };
    constexpr uint32_t kTransmittanceWidth = 256, kTransmittanceHeight = 64, kMultipleSide = 32, kIrradianceSize = 64;
    constexpr float kIrradianceLow = -0.35f;

    Atmosphere make_atmosphere(const SkySettings &settings);

    // The light's transmittance through the atmosphere to the viewer, per channel: what of a light above the
    // atmosphere reaches it along toward_light (unit, y up); 0 once the planet hides the light.
    Vec3f sky_transmittance(const Atmosphere &atmosphere, const Vec3f &toward_light);
    // The same without the tables, by integrating the air along the ray, for a light toward_y above the horizon (its sine).
    Vec3f sun_transmittance(const SkySettings &settings, float toward_y);

    // The sky's radiance seen along view (unit, y up), for a light of unit illuminance above the atmosphere along
    // toward_light, the light's own disc left out. Below the horizon it is the lit ground seen through the air.
    Vec3f sky_radiance(const Atmosphere &atmosphere, const Vec3f &view, const Vec3f &toward_light);

    // A light above the atmosphere: the sun, or the moon by the sunlight it sends back. Its illuminance there is in the
    // sun's: 1 for the sun.
    struct SkyLight
    {
        Vec3f toward {};            // unit
        float illuminance = 0;
    };

    // The moon's illuminance above the atmosphere over the sun's at a phase angle in radians, 0 full: Allen's magnitudes,
    // -12.73 full, fading by 0.026 a degree and the fourth power of the angle.
    float moon_illuminance(float phase_angle);

    // The moon's angular radius at its mean distance, in radians.
    constexpr float kMoonRadius = 0.004528f;
    // The drawn moon's width over the real one's, about 9 degrees across as games draw it.
    constexpr float kMoonDrawn = 20;

    // The sunlit moon's radiance over its albedo (its mean's multiple) and its Lommel-Seeliger shading
    // cos i / (cos i + cos e), in the sun's units, at a phase angle: the disc then sends moon_illuminance's light, the
    // Lommel-Seeliger disc's phase law 1 - sin(a/2) tan(a/2) ln(cot(a/4)) taking the rest.
    float moon_radiance(float phase_angle);

    // The earthlit moon's radiance over its albedo: the ashen light, 1.7e-4 of the full moon's at a full earth, which wanes
    // as the earth's phase seen from the moon, the moon's opposite.
    float earthshine_radiance(float phase_angle);

    // The moonless night sky's own light along view, per unit of the sun's illuminance: airglow, brighter toward the
    // horizon as its layer 90 km up is seen edge on, and the faint background of unresolved stars and zodiacal light,
    // both through the air's transmittance; 22 magnitudes per square arcsecond at the zenith in all. Zero below the horizon.
    // A Milky Way would add here.
    Vec3f night_glow(const Atmosphere &atmosphere, const Vec3f &view);

    // The luminance a level floor gets from a light, straight and from the sky it lights, per unit of the sun's illuminance.
    float level_illuminance(const Atmosphere &atmosphere, const SkyLight &light);
    // The same from the sky alone.
    float sky_floor(const Atmosphere &atmosphere, const SkyLight &light);

    // How the physical sky shows in the game's units until the night has an exposure of its own: the sun straight
    // overhead lights a white surface facing it as the game's day light does, white, so the sun's color through the air
    // at the zenith is the white balance. Once the light on a level floor falls below the sun's at 10 degrees up, the
    // exposure follows it by 0.85 (1 would keep a floor as bright as then), so the blue hour stays blue, and once it falls
    // below the sun's at 6 degrees down, by night_power, which Night brightness sets; for at most night_stops stops.
    struct SkyExposure
    {
        float day_light = 1.5f;     // the luminance the game's day sun shows on a white surface facing it
        float night_power = 0.85f;
        float night_stops = 17;
    };

    // The provisional exposure's multiple over a day's: 1 until the light on a level floor falls below the sun's at 10 degrees.
    float exposure_gain(const Atmosphere &atmosphere, const SkyLight &sun, const SkyLight &moon, const SkyExposure &exposure);

    // How many stops darker than at the blue hour's end, the sun 6 degrees down, the exposure shows a night: 0 till then,
    // and with night_power below 1 more the darker the night, above 1 brighter. Auto exposure aims that far off middle
    // gray, so it keeps the blue hour bright and leaves the night as Night brightness makes it.
    float night_stops_under_dusk(const Atmosphere &atmosphere, const SkyLight &sun, const SkyLight &moon, const SkyExposure &exposure);

    // Game units per unit of the sun's light above the atmosphere, per channel.
    Vec3f sky_scale(const Atmosphere &atmosphere, const SkyLight &sun, const SkyLight &moon, const SkyExposure &exposure);

    // The transmittance from altitude_km above the ground toward a light above the air, the planet's shadow included.
    Vec3f transmittance_at(const Atmosphere &atmosphere, float altitude_km, const Vec3f &toward);

    // What a physical sky is built for.
    struct SkyRequest
    {
        SkyLight sun, moon;
        SkyExposure exposure;
        float haze = 0.1f;          // SkySettings::aerosol_depth
        float sunset = 0;           // SkySettings::sunset
    };

    // How the physical sky lights the world, in the game's units at sky_scale, so the world takes the drawn sky's exposure.
    struct WorldLight
    {
        Vec3f sun, moon;            // each body's direct light on a white surface facing it
        float sky_share = 0;        // the sky's light on a level floor over its light with the sun overhead at a day's exposure
    };
    WorldLight world_light(const Atmosphere &atmosphere, const SkyRequest &request);

    // The sky's light along view in the game's units for scale: the sun's and the moon's light scattered by the air, and
    // the night's own.
    Vec3f sky_light(const Atmosphere &atmosphere, const Vec3f &view, const SkyRequest &request, const Vec3f &scale);

    // The light that lights clouds altitude_km above the ground: the sun while its light there passes the moon's, else
    // the moon.
    SkyLight cloud_light(const Atmosphere &atmosphere, const SkyRequest &request, float altitude_km);

    // The physical sky as the tracer's sky map, at sky_scale. Every texel is real.
    SkyMap physical_sky_map(const Atmosphere &atmosphere, const SkyRequest &request);

    // The mean of the map's texels from the horizon to 10 degrees above it, all around: the light distant haze takes.
    Vec3f sky_horizon(const SkyMap &sky);

    // Whether two requests build skies far enough apart to build again: a light's direction a quarter of a degree, its
    // illuminance 1%, the haze or the exposure at all.
    bool sky_request_differs(const SkyRequest &a, const SkyRequest &b);

    struct SkyResult
    {
        SkyRequest request;
        std::shared_ptr<const Atmosphere> atmosphere;   // its lookup tables, for the drawn sky
        Vec3f scale {};             // sky_scale's
        SkyMap map;
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
