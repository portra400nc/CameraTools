#include "sky.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace enhancements
{
    namespace
    {
        constexpr float kPi = 3.14159265358979f;
        // Kilometers, and coefficients per kilometer: Hillaire's Earth.
        constexpr float kGround = 6360, kTop = 6460;
        constexpr Vec3f kRayleigh = { 5.802e-3f, 13.558e-3f, 33.1e-3f };
        constexpr float kRayleighHeight = 8;
        constexpr float kMieScatter = 3.996e-3f, kMieExtinction = 4.40e-3f, kMieHeight = 1.2f, kMieG = 0.8f;
        // Ozone's red is Hillaire's 680 nm one raised to the sRGB red response's weight near the Chappuis band's 600 nm
        // peak: at 680 nm the red channel passed the long twilight paths and turned the blue hour purple.
        constexpr Vec3f kOzone = { 1.85e-3f, 1.881e-3f, 0.085e-3f };
        constexpr float kOzoneCenter = 25, kOzoneHalfWidth = 15;

        Vec3f add(const Vec3f &a, const Vec3f &b) { return { a[0] + b[0], a[1] + b[1], a[2] + b[2] }; }
        Vec3f mul(const Vec3f &a, const Vec3f &b) { return { a[0] * b[0], a[1] * b[1], a[2] * b[2] }; }
        Vec3f scale(const Vec3f &a, float s) { return { a[0] * s, a[1] * s, a[2] * s }; }
        Vec3f exp_of(const Vec3f &a) { return { std::exp(a[0]), std::exp(a[1]), std::exp(a[2]) }; }
        float dot(const Vec3f &a, const Vec3f &b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
        float luminance_of(const Vec3f &c) { return 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2]; }
        float clamp01(float x) { return std::min(std::max(x, 0.0f), 1.0f); }

        struct Medium
        {
            Vec3f rayleigh;     // scattering
            float mie;          // scattering
            Vec3f extinction;
        };

        Medium medium(const SkySettings &s, float height)
        {
            const float r = std::exp(-height / kRayleighHeight), m = std::exp(-height / kMieHeight) * s.aerosol_depth / (kMieExtinction * kMieHeight);
            const float o = std::max(0.0f, 1 - std::fabs(height - kOzoneCenter) / kOzoneHalfWidth);
            Medium out;
            out.rayleigh = scale(kRayleigh, r);
            out.mie = kMieScatter * m;
            out.extinction = add(add(out.rayleigh, Vec3f { kMieExtinction * m, kMieExtinction * m, kMieExtinction * m }), scale(kOzone, o * s.ozone));
            return out;
        }

        float rayleigh_phase(float c) { return 3 / (16 * kPi) * (1 + c * c); }

        // Cornette-Shanks.
        float mie_phase(float c)
        {
            const float g2 = kMieG * kMieG;
            return 3 / (8 * kPi) * (1 - g2) * (1 + c * c) / ((2 + g2) * std::pow(1 + g2 - 2 * kMieG * c, 1.5f));
        }

        float distance_to_top(float r, float mu) { return -r * mu + std::sqrt(std::max(r * r * (mu * mu - 1) + kTop * kTop, 0.0f)); }

        // The distance to the ground along mu, or a negative number when the ray misses it.
        float distance_to_ground(float r, float mu)
        {
            const float d = r * r * (mu * mu - 1) + kGround * kGround;
            return mu < 0 && d >= 0 ? -r * mu - std::sqrt(d) : -1;
        }

        // How much of the light's disc the planet leaves above the horizon at radius r, for the light's cosine mu.
        float light_visible(float r, float mu)
        {
            const float horizon = -std::sqrt(std::max(1 - (kGround / r) * (kGround / r), 0.0f));
            const float t = clamp01((mu - horizon + 0.005f) / 0.01f);
            return t * t * (3 - 2 * t);
        }

        // Bruneton's mapping of (r, mu) for the transmittance table, which spends its rows near the horizon.
        void transmittance_uv(float r, float mu, float &u, float &v)
        {
            const float h = std::sqrt(kTop * kTop - kGround * kGround), rho = std::sqrt(std::max(r * r - kGround * kGround, 0.0f));
            const float d = distance_to_top(r, mu), d_min = kTop - r, d_max = rho + h;
            u = (d - d_min) / std::max(d_max - d_min, 1e-6f);
            v = rho / h;
        }

        void transmittance_r_mu(float u, float v, float &r, float &mu)
        {
            const float h = std::sqrt(kTop * kTop - kGround * kGround), rho = h * v;
            r = std::sqrt(rho * rho + kGround * kGround);
            const float d_min = kTop - r, d_max = rho + h, d = d_min + u * (d_max - d_min);
            mu = d == 0 ? 1.0f : std::min(std::max((h * h - rho * rho - d * d) / (2 * r * d), -1.0f), 1.0f);
        }

        Vec3f bilinear(const std::vector<Vec3f> &table, uint32_t width, uint32_t height, float u, float v)
        {
            const float x = std::min(std::max(u * width - 0.5f, 0.0f), float(width - 1)), y = std::min(std::max(v * height - 0.5f, 0.0f), float(height - 1));
            const uint32_t x0 = uint32_t(x), y0 = uint32_t(y), x1 = std::min(x0 + 1, width - 1), y1 = std::min(y0 + 1, height - 1);
            const float fx = x - x0, fy = y - y0;
            const Vec3f top = add(scale(table[y0 * width + x0], 1 - fx), scale(table[y0 * width + x1], fx));
            const Vec3f bottom = add(scale(table[y1 * width + x0], 1 - fx), scale(table[y1 * width + x1], fx));
            return add(scale(top, 1 - fy), scale(bottom, fy));
        }

        Vec3f transmittance_to_top(const Atmosphere &a, float r, float mu)
        {
            float u, v;
            transmittance_uv(r, mu, u, v);
            return bilinear(a.transmittance, kTransmittanceWidth, kTransmittanceHeight, u, v);
        }

        // The light reaching radius r with cosine mu, the planet's shadow included.
        Vec3f light_at(const Atmosphere &a, float r, float mu) { return scale(transmittance_to_top(a, r, mu), light_visible(r, mu)); }

        Vec3f multiple_at(const Atmosphere &a, float r, float mu)
        {
            return bilinear(a.multiple, kMultipleSide, kMultipleSide, mu * 0.5f + 0.5f, (r - kGround) / (kTop - kGround));
        }

        // What a march along a ray adds over one step of length dt through a medium whose in-scattered light is
        // inscatter: the light scattered toward the start, through the step's own extinction.
        Vec3f step_light(const Vec3f &inscatter, const Vec3f &extinction, float dt)
        {
            Vec3f out;
            for (int c = 0; c < 3; c++)
                out[c] = extinction[c] > 1e-9f ? inscatter[c] * (1 - std::exp(-extinction[c] * dt)) / extinction[c] : inscatter[c] * dt;
            return out;
        }

        // The point t along dir from radius r0 straight up: its radius and the up direction there.
        void point_on(float r0, const Vec3f &dir, float t, float &r, Vec3f &up)
        {
            const Vec3f p = { dir[0] * t, r0 + dir[1] * t, dir[2] * t };
            r = std::sqrt(dot(p, p));
            up = scale(p, 1 / r);
        }

        // The transmittance from radius r along mu to the atmosphere's top, the planet's shadow left out.
        Vec3f integrate_transmittance(const SkySettings &settings, float r, float mu)
        {
            const float length = distance_to_top(r, mu);
            constexpr int kSteps = 400;
            Vec3f depth {};
            for (int k = 0; k < kSteps; k++)
            {
                const float t = (k + 0.5f) * length / kSteps;
                const float rk = std::sqrt(r * r + t * t + 2 * r * mu * t);
                depth = add(depth, scale(medium(settings, rk - kGround).extinction, length / kSteps));
            }
            return exp_of(scale(depth, -1));
        }

        // The stylized sunset: light the air reddened, by how much its blue fell behind its red, gains blue from its red
        // and loses some green, so a peach or orange sun turns pink; white light keeps its color.
        Vec3f rose(const SkySettings &s, const Vec3f &t)
        {
            if (s.sunset <= 0 || t[0] <= 0)
                return t;
            const float k = s.sunset * clamp01(1 - t[2] / t[0]);
            return { t[0], t[1] * (1 - 0.35f * k), std::min(t[2] + 0.6f * k * t[0], 1.0f) };
        }

        void make_transmittance(Atmosphere &a)
        {
            a.transmittance.resize(size_t(kTransmittanceWidth) * kTransmittanceHeight);
            for (uint32_t y = 0; y < kTransmittanceHeight; y++)
                for (uint32_t x = 0; x < kTransmittanceWidth; x++)
                {
                    float r, mu;
                    transmittance_r_mu((x + 0.5f) / kTransmittanceWidth, (y + 0.5f) / kTransmittanceHeight, r, mu);
                    a.transmittance[size_t(y) * kTransmittanceWidth + x] = rose(a.settings, integrate_transmittance(a.settings, r, mu));
                }
        }

        // Hillaire's multiple scattering: at each height and light cosine, the second order's light and the share of
        // light the medium scatters, over 64 directions spread evenly over the sphere with the isotropic phase, summed as
        // a geometric series.
        void make_multiple(Atmosphere &a)
        {
            constexpr int kDirections = 64, kSteps = 32;
            std::array<Vec3f, kDirections> directions;
            for (int i = 0; i < kDirections; i++)
            {
                const float y = 1 - (i + 0.5f) * 2 / kDirections, ring = std::sqrt(std::max(1 - y * y, 0.0f));
                const float phi = i * 2.39996323f;
                directions[size_t(i)] = { ring * std::cos(phi), y, ring * std::sin(phi) };
            }
            const float isotropic = 1 / (4 * kPi);
            a.multiple.resize(size_t(kMultipleSide) * kMultipleSide);
            for (uint32_t y = 0; y < kMultipleSide; y++)
                for (uint32_t x = 0; x < kMultipleSide; x++)
                {
                    const float mu_light = (x + 0.5f) / kMultipleSide * 2 - 1;
                    const float r0 = kGround + std::max((y + 0.5f) / kMultipleSide * (kTop - kGround), 0.01f);
                    const Vec3f light = { std::sqrt(std::max(1 - mu_light * mu_light, 0.0f)), mu_light, 0 };
                    Vec3f second {}, share {};
                    for (const Vec3f &dir : directions)
                    {
                        const float ground = distance_to_ground(r0, dir[1]);
                        const float length = ground >= 0 ? ground : distance_to_top(r0, dir[1]);
                        Vec3f throughput = { 1, 1, 1 }, lit {}, scattered {};
                        float previous = 0;
                        for (int k = 1; k <= kSteps; k++)
                        {
                            const float f = float(k) / kSteps, t = length * f * f, dt = t - previous;
                            float r;
                            Vec3f up;
                            point_on(r0, dir, previous + dt * 0.5f, r, up);
                            previous = t;
                            const Medium m = medium(a.settings, r - kGround);
                            const Vec3f scatter = add(m.rayleigh, Vec3f { m.mie, m.mie, m.mie });
                            const Vec3f sun = light_at(a, r, dot(up, light));
                            lit = add(lit, mul(throughput, step_light(scale(mul(scatter, sun), isotropic), m.extinction, dt)));
                            scattered = add(scattered, mul(throughput, step_light(scatter, m.extinction, dt)));
                            throughput = mul(throughput, exp_of(scale(m.extinction, -dt)));
                        }
                        if (ground >= 0)
                        {
                            float r;
                            Vec3f up;
                            point_on(r0, dir, ground, r, up);
                            const float mu = dot(up, light);
                            lit = add(lit, mul(throughput, scale(light_at(a, r, mu), a.settings.ground_albedo / kPi * std::max(mu, 0.0f))));
                        }
                        second = add(second, scale(lit, 1.0f / kDirections));
                        share = add(share, scale(scattered, 1.0f / kDirections));
                    }
                    Vec3f psi;
                    for (int c = 0; c < 3; c++)
                        psi[c] = second[c] / std::max(1 - share[c], 1e-3f);
                    a.multiple[size_t(y) * kMultipleSide + x] = psi;
                }
        }
    }

    namespace
    {
        // The light's direction at a sine of its elevation, along +x.
        Vec3f toward_sine(float sine) { return { std::sqrt(std::max(1 - sine * sine, 0.0f)), sine, 0 }; }

        // The sky's light on a level floor over 128 directions spread evenly over the upper half, each weighted by its
        // cosine.
        void make_irradiance(Atmosphere &a)
        {
            constexpr int kDirections = 128;
            a.irradiance.resize(kIrradianceSize);
            for (uint32_t i = 0; i < kIrradianceSize; i++)
            {
                const Vec3f light = toward_sine(kIrradianceLow + (1 - kIrradianceLow) * i / (kIrradianceSize - 1));
                float sum = 0;
                for (int k = 0; k < kDirections; k++)
                {
                    const float y = 1 - (k + 0.5f) / kDirections, ring = std::sqrt(std::max(1 - y * y, 0.0f)), phi = k * 2.39996323f;
                    sum += luminance_of(sky_radiance(a, { ring * std::cos(phi), y, ring * std::sin(phi) }, light)) * y;
                }
                a.irradiance[i] = sum * 2 * kPi / kDirections;
            }
        }
    }

    Atmosphere make_atmosphere(const SkySettings &settings)
    {
        Atmosphere a;
        a.settings = settings;
        a.settings.aerosol_depth = std::min(std::max(settings.aerosol_depth, 0.0f), 2.0f);
        a.settings.ground_albedo = clamp01(settings.ground_albedo);
        a.settings.altitude_km = std::min(std::max(settings.altitude_km, 0.001f), 50.0f);
        a.settings.ozone = std::min(std::max(settings.ozone, 0.0f), 10.0f);
        a.settings.sunset = clamp01(settings.sunset);
        make_transmittance(a);
        make_multiple(a);
        make_irradiance(a);
        return a;
    }

    Vec3f sun_transmittance(const SkySettings &settings, float toward_y)
    {
        const float r = kGround + std::min(std::max(settings.altitude_km, 0.001f), 50.0f), mu = std::min(std::max(toward_y, -1.0f), 1.0f);
        SkySettings clamped = settings;
        clamped.aerosol_depth = std::min(std::max(settings.aerosol_depth, 0.0f), 2.0f);
        return scale(rose(clamped, integrate_transmittance(clamped, r, mu)), light_visible(r, mu));
    }

    Vec3f sky_transmittance(const Atmosphere &atmosphere, const Vec3f &toward_light)
    {
        return light_at(atmosphere, kGround + atmosphere.settings.altitude_km, toward_light[1]);
    }

    Vec3f sky_radiance(const Atmosphere &a, const Vec3f &view, const Vec3f &toward_light)
    {
        const float r0 = kGround + a.settings.altitude_km;
        const float ground = distance_to_ground(r0, view[1]);
        const float length = ground >= 0 ? ground : distance_to_top(r0, view[1]);
        const float c = dot(view, toward_light), rayleigh = rayleigh_phase(c), mie = mie_phase(c);
        // Steps that grow with the distance, as the air thins away from the viewer.
        constexpr int kSteps = 48;
        Vec3f throughput = { 1, 1, 1 }, light {};
        float previous = 0;
        for (int k = 1; k <= kSteps; k++)
        {
            const float f = float(k) / kSteps, t = length * f * f, dt = t - previous;
            float r;
            Vec3f up;
            point_on(r0, view, previous + dt * 0.5f, r, up);
            previous = t;
            const Medium m = medium(a.settings, r - kGround);
            const float mu = dot(up, toward_light);
            const Vec3f sun = light_at(a, r, mu), psi = multiple_at(a, r, mu);
            const Vec3f inscatter = add(mul(m.rayleigh, add(scale(sun, rayleigh), psi)), scale(add(scale(sun, mie), psi), m.mie));
            light = add(light, mul(throughput, step_light(inscatter, m.extinction, dt)));
            throughput = mul(throughput, exp_of(scale(m.extinction, -dt)));
        }
        if (ground >= 0)
        {
            float r;
            Vec3f up;
            point_on(r0, view, ground, r, up);
            const float mu = dot(up, toward_light);
            light = add(light, mul(throughput, scale(light_at(a, r, mu), a.settings.ground_albedo / kPi * std::max(mu, 0.0f))));
        }
        return light;
    }

    float moon_illuminance(float phase_angle)
    {
        const float degrees = std::fabs(phase_angle) * 180 / kPi;
        const float magnitude = -12.73f + 0.026f * degrees + 4e-9f * degrees * degrees * degrees * degrees;
        return std::pow(10.0f, -0.4f * (magnitude + 26.74f));
    }

    float moon_radiance(float phase_angle)
    {
        const float a = std::min(std::fabs(phase_angle), kPi);
        const float law = a < 1e-4f ? 1 : 1 - std::sin(a / 2) * std::tan(a / 2) * std::log(1 / std::tan(a / 4));
        return 2 * moon_illuminance(a) / (kPi * kMoonRadius * kMoonRadius * std::max(law, 1e-3f));
    }

    float earthshine_radiance(float phase_angle)
    {
        const float earth = kPi - std::min(std::fabs(phase_angle), kPi);
        return 1.7e-4f * moon_radiance(0) * 0.5f * (std::sin(earth) + (kPi - earth) * std::cos(earth)) / kPi;
    }

    Vec3f transmittance_at(const Atmosphere &a, float altitude_km, const Vec3f &toward)
    {
        return light_at(a, kGround + std::min(std::max(altitude_km, 0.001f), kTop - kGround - 0.001f), toward[1]);
    }

    SkyLight cloud_light(const Atmosphere &a, const SkyRequest &request, float altitude_km)
    {
        altitude_km += request.sunset * kSunsetLiftKm;
        const float sun = request.sun.illuminance * luminance_of(transmittance_at(a, altitude_km, request.sun.toward));
        const float moon = request.moon.illuminance * luminance_of(transmittance_at(a, altitude_km, request.moon.toward));
        return sun >= moon ? request.sun : request.moon;
    }

    Vec3f night_glow(const Atmosphere &a, const Vec3f &view)
    {
        if (view[1] <= 0)
            return {};
        // 22 magnitudes per square arcsecond, a little over half of it airglow, over the sun's -26.74 and a square
        // arcsecond's 2.35e-11 steradians.
        constexpr float kZenith = 1.35e-9f, kAirglow = 0.6f;
        // A night blue at luminance 1, as the eye sees a moonless sky: a camera finds oxygen's green 557.7 nm line, which
        // Night brightness's exposure turned into an olive sky.
        constexpr Vec3f kAirglowColor = { 0.80f, 1.0f, 1.45f };
        const float ratio = kGround / (kGround + 90), edge_on = 1 / std::sqrt(std::max(1 - ratio * ratio * (1 - view[1] * view[1]), 1e-4f));
        const float background = kZenith * (1 - kAirglow);
        const Vec3f glow = add(scale(kAirglowColor, kZenith * kAirglow * edge_on), Vec3f { background, background, background });
        return mul(glow, sky_transmittance(a, view));
    }

    float sky_floor(const Atmosphere &a, const SkyLight &light)
    {
        if (light.illuminance <= 0)
            return 0;
        const float sine = std::min(std::max(light.toward[1], -1.0f), 1.0f);
        const float x = std::min(std::max((sine - kIrradianceLow) / (1 - kIrradianceLow), 0.0f), 1.0f) * (kIrradianceSize - 1);
        const uint32_t i0 = std::min(uint32_t(x), kIrradianceSize - 2);
        return light.illuminance * (a.irradiance[i0] + (a.irradiance[i0 + 1] - a.irradiance[i0]) * (x - i0));
    }

    float level_illuminance(const Atmosphere &a, const SkyLight &light)
    {
        if (light.illuminance <= 0)
            return 0;
        const float direct = luminance_of(sky_transmittance(a, light.toward)) * std::max(light.toward[1], 0.0f);
        return sky_floor(a, light) + light.illuminance * direct;
    }

    namespace
    {
        // A moonless night's floor gets about pi times the glow's zenith radiance.
        float night_floor(const Atmosphere &a) { return kPi * luminance_of(night_glow(a, { 0, 1, 0 })); }
    }

    float exposure_gain(const Atmosphere &a, const SkyLight &sun, const SkyLight &moon, const SkyExposure &exposure)
    {
        // A low moon lights the level ground little, yet its beam, its glow and what faces it stay as bright as a high moon's:
        // the night takes the moon's light on a surface facing it where that is more, so a low moon is not exposed toward
        // a moonless night's brightness, which washed the sky out to a grey day around a glowing moon.
        const float facing_moon = moon.illuminance > 0 && moon.toward[1] > 0 ? moon.illuminance * luminance_of(sky_transmittance(a, moon.toward)) : 0;
        const float light = level_illuminance(a, sun) + std::max(level_illuminance(a, moon), facing_moon) + night_floor(a);
        const float low_sun = level_illuminance(a, { { 0.98481f, 0.17365f, 0 }, 1 });
        const float noon = level_illuminance(a, { { 0, 1, 0 }, 1 });
        if (!(light > 0))
            return 1;
        // Through the day the exposure follows the light by kDayAdaptation, as an eye or a camera does, so a morning sun
        // does not leave the picture dark without the auto exposure; the noon sun keeps the day's exposure.
        if (light >= low_sun)
            return std::max(std::pow(noon / light, kDayAdaptation), 1.0f);
        // Below it the day's lift fades out by the light's fall toward the sun's at 6 degrees down, so the blue hour and the
        // night keep the exposure they were tuned with.
        const float dusk = level_illuminance(a, { { 0.99452f, -0.10453f, 0 }, 1 }) + night_floor(a);
        const float fade = clamp01(std::log(std::max(light, dusk) / dusk) / std::log(low_sun / dusk));
        float gain = std::pow(noon / low_sun, kDayAdaptation * fade) * std::pow(low_sun / std::max(light, dusk), 0.85f);
        if (light < dusk)
            gain *= std::pow(dusk / light, std::max(exposure.night_power, 0.0f));
        // The lift auto exposure gave the blue hour, taken over as it fades out, so the night follows on without a jump.
        gain *= std::exp2(kNightLiftStops * (1 - auto_exposure_share(sun)));
        return std::min(gain, std::exp2(std::max(exposure.night_stops, 0.0f)));
    }

    float auto_exposure_share(const SkyLight &sun)
    {
        const float degrees = std::asin(std::min(std::max(sun.toward[1], -1.0f), 1.0f)) * 180 / kPi;
        return clamp01((degrees + 12) / 6);
    }

    Vec3f sky_scale(const Atmosphere &a, const SkyLight &sun, const SkyLight &moon, const SkyExposure &exposure)
    {
        const Vec3f zenith = sky_transmittance(a, { 0, 1, 0 });
        const float white = luminance_of(zenith);
        const float k = kPi * std::max(exposure.day_light, 0.0f) / std::max(white, 1e-6f) * exposure_gain(a, sun, moon, exposure);
        return { k * white / zenith[0], k * white / zenith[1], k * white / zenith[2] };
    }

    WorldLight world_light(const Atmosphere &a, const SkyRequest &request)
    {
        const Vec3f k = sky_scale(a, request.sun, request.moon, request.exposure);
        const auto direct = [&](const SkyLight &light) {
            return light.illuminance > 0 ? scale(mul(sky_transmittance(a, light.toward), k), light.illuminance / kPi) : Vec3f {};
        };
        // The sun lights the world a little warmer than the white balance, about 5200 K, its luminance kept.
        const Vec3f sun = mul(direct(request.sun), kSunWarmth);
        const float day = sky_floor(a, { { 0, 1, 0 }, 1 });
        const float now = sky_floor(a, request.sun) + sky_floor(a, request.moon) + night_floor(a);
        return { sun, scale(direct(request.moon), kMoonOnWorld), day > 0 ? now * exposure_gain(a, request.sun, request.moon, request.exposure) / day : 0 };
    }

    Vec3f sky_light(const Atmosphere &a, const Vec3f &view, const SkyRequest &request, const Vec3f &k)
    {
        Vec3f light = night_glow(a, view);
        // A light below 20 degrees down, or one a ten-thousandth of the other's, adds nothing that shows.
        const float sun = level_illuminance(a, request.sun), moon = level_illuminance(a, request.moon);
        if (request.sun.illuminance > 0 && request.sun.toward[1] > -0.35f && sun >= 1e-4f * moon)
            light = add(light, scale(sky_radiance(a, view, request.sun.toward), request.sun.illuminance));
        if (request.moon.illuminance > 0 && request.moon.toward[1] > -0.35f && moon >= 1e-4f * sun)
            light = add(light, scale(sky_radiance(a, view, request.moon.toward), request.moon.illuminance));
        return mul(light, k);
    }

    SkyMap physical_sky_map(const Atmosphere &atmosphere, const SkyRequest &request)
    {
        const Vec3f k = sky_scale(atmosphere, request.sun, request.moon, request.exposure);
        SkyMap sky;
        sky.texels.assign(size_t(kSkyMapSide) * kSkyMapSide * 4, 0.0f);
        for (uint32_t y = 0; y < kSkyMapSide; y++)
            for (uint32_t x = 0; x < kSkyMapSide; x++)
            {
                Vec3f sum {};
                for (uint32_t s = 0; s < 4; s++)
                {
                    const Vec3f d = sky_direction((x + (s % 2 + 0.5f) / 2) / kSkyMapSide, (y + (s / 2 + 0.5f) / 2) / kSkyMapSide);
                    sum = add(sum, sky_light(atmosphere, d, request, k));
                }
                float *texel = &sky.texels[(size_t(y) * kSkyMapSide + x) * 4];
                for (int c = 0; c < 3; c++)
                    texel[c] = sum[c] / 4;
                texel[3] = 1;
            }
        sky.real = kSkyMapSide * kSkyMapSide;
        set_sky_harmonics(sky);
        return sky;
    }

    Vec3f sky_horizon(const SkyMap &sky)
    {
        Vec3f sum {};
        uint32_t count = 0;
        const float top = std::sin(10 * kPi / 180);
        for (uint32_t y = 0; y < kSkyMapSide; y++)
            for (uint32_t x = 0; x < kSkyMapSide; x++)
            {
                const Vec3f d = sky_direction((x + 0.5f) / kSkyMapSide, (y + 0.5f) / kSkyMapSide);
                if (d[1] < 0 || d[1] > top)
                    continue;
                const float *t = &sky.texels[(size_t(y) * kSkyMapSide + x) * 4];
                sum = add(sum, { t[0], t[1], t[2] });
                count++;
            }
        return count ? scale(sum, 1.0f / count) : sum;
    }

    bool sky_request_differs(const SkyRequest &a, const SkyRequest &b)
    {
        const auto light_differs = [](const SkyLight &x, const SkyLight &y)
        {
            return dot(x.toward, y.toward) < 0.9999905f || std::fabs(x.illuminance - y.illuminance) > 0.01f * std::max(x.illuminance, y.illuminance);
        };
        return light_differs(a.sun, b.sun) || light_differs(a.moon, b.moon) || a.haze != b.haze || a.sunset != b.sunset || a.exposure.day_light != b.exposure.day_light
            || a.exposure.night_power != b.exposure.night_power || a.exposure.night_stops != b.exposure.night_stops;
    }

    std::string sky_request_change(const SkyRequest &a, const SkyRequest &b)
    {
        std::string out;
        char text[96];
        const auto add = [&out](const char *what) { out += (out.empty() ? "" : ", ") + std::string(what); };
        const auto light = [&](const char *name, const SkyLight &x, const SkyLight &y) {
            const float cosine = std::clamp(dot(x.toward, y.toward), -1.0f, 1.0f);
            if (cosine < 0.9999905f)
            {
                std::snprintf(text, sizeof text, "%s direction %.3g degrees", name, std::acos(cosine) * 57.29578f);
                add(text);
            }
            const float most = std::max(x.illuminance, y.illuminance);
            if (std::fabs(x.illuminance - y.illuminance) > 0.01f * most)
            {
                std::snprintf(text, sizeof text, "%s illuminance %.3g%%", name, most > 0 ? 100 * std::fabs(x.illuminance - y.illuminance) / most : 0.0f);
                add(text);
            }
        };
        light("sun", a.sun, b.sun);
        light("moon", a.moon, b.moon);
        if (a.haze != b.haze)
            add("haze");
        if (a.sunset != b.sunset)
            add("sunset");
        if (a.exposure.day_light != b.exposure.day_light || a.exposure.night_power != b.exposure.night_power || a.exposure.night_stops != b.exposure.night_stops)
            add("exposure");
        return out;
    }

    SkyBuilder::SkyBuilder() : thread_([this] { run(); }) {}

    SkyBuilder::~SkyBuilder()
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_ = true;
        }
        wake_.notify_one();
        thread_.join();
    }

    void SkyBuilder::request(const SkyRequest &request)
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            pending_ = request;
        }
        wake_.notify_one();
    }

    std::shared_ptr<const SkyResult> SkyBuilder::latest() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return latest_;
    }

    void SkyBuilder::run()
    {
        std::shared_ptr<const Atmosphere> air;
        uint64_t version = 0;
        for (;;)
        {
            SkyRequest request;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                wake_.wait(lock, [this] { return stop_ || pending_.has_value(); });
                if (stop_)
                    return;
                request = *pending_;
                pending_.reset();
            }
            if (!air || air->settings.aerosol_depth != std::min(std::max(request.haze, 0.0f), 2.0f) || air->settings.sunset != request.sunset)
            {
                SkySettings settings;
                settings.aerosol_depth = request.haze;
                settings.sunset = request.sunset;
                air = std::make_shared<const Atmosphere>(make_atmosphere(settings));
            }
            auto result = std::make_shared<SkyResult>();
            result->request = request;
            result->atmosphere = air;
            result->scale = sky_scale(*air, request.sun, request.moon, request.exposure);
            result->map = physical_sky_map(*air, request);
            result->horizon = sky_horizon(result->map);
            result->version = ++version;
            std::lock_guard<std::mutex> lock(mutex_);
            latest_ = std::move(result);
        }
    }

    namespace
    {
        // A uniform 0 to 1 from an integer and a stream, by PCG's output mix.
        float lightning_hash(int64_t n, uint32_t stream)
        {
            uint32_t v = static_cast<uint32_t>(n) * 747796405u + 2891336453u + stream * 2654435761u;
            v ^= static_cast<uint32_t>(static_cast<uint64_t>(n) >> 32) * 1664525u;
            const uint32_t word = ((v >> ((v >> 28u) + 4u)) ^ v) * 277803737u;
            return static_cast<float>((word >> 22u) ^ word) / 4294967296.0f;
        }
    }

    Lightning lightning_at(double seconds, float rate)
    {
        Lightning result;
        if (!(rate > 0) || !std::isfinite(seconds))
            return result;
        // Each second may hold a strike; one lasts under two seconds, so the two before this one may still glow.
        const int64_t now = static_cast<int64_t>(std::floor(seconds));
        for (int64_t slot = now - 2; slot <= now; slot++)
        {
            if (lightning_hash(slot, 0) >= std::min(rate, 1.0f) / 7)
                continue;
            const float near = lightning_hash(slot, 1), shape = lightning_hash(slot, 2);
            const double start = static_cast<double>(slot) + 0.6 * lightning_hash(slot, 3);
            const float t = static_cast<float>(seconds - start);
            if (t < 0)
                continue;
            // A return stroke and its restrikes: flickers a tenth of a second or so apart, then a dim afterglow in the clouds.
            const float flickers[3][2] = { { 0, 1 }, { 0.08f + 0.06f * shape, 0.6f }, { 0.2f + 0.12f * shape, 0.4f + 0.5f * near } };
            float flash = 0.15f * std::exp(-t / 0.4f);
            for (const auto &f : flickers)
                if (t >= f[0])
                    flash += f[1] * std::exp(-(t - f[0]) / 0.05f);
            // Some strikes are distant, a dim flash with no bolt in view.
            flash *= 0.35f + 0.65f * near;
            if (flash > result.flash)
            {
                result.flash = flash;
                result.bolt = near > 0.35f ? flash : 0;
                // Most strikes fall ahead, where the camera looks: u |u| crowds the azimuths toward 0.
                const float u = 2 * lightning_hash(slot, 4) - 1;
                result.azimuth = 3.1415927f * u * std::fabs(u);
                result.seed = 100 * lightning_hash(slot, 5);
            }
        }
        result.flash = std::min(result.flash, 1.5f);
        result.bolt = result.bolt > 0.02f ? std::min(result.bolt, 1.5f) : 0;
        return result;
    }
}
