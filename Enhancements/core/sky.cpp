#include "sky.h"

#include <algorithm>
#include <cmath>

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
        constexpr Vec3f kOzone = { 0.650e-3f, 1.881e-3f, 0.085e-3f };
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
            out.extinction = add(add(out.rayleigh, Vec3f { kMieExtinction * m, kMieExtinction * m, kMieExtinction * m }), scale(kOzone, o));
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

        void make_transmittance(Atmosphere &a)
        {
            a.transmittance.resize(size_t(kTransmittanceWidth) * kTransmittanceHeight);
            for (uint32_t y = 0; y < kTransmittanceHeight; y++)
                for (uint32_t x = 0; x < kTransmittanceWidth; x++)
                {
                    float r, mu;
                    transmittance_r_mu((x + 0.5f) / kTransmittanceWidth, (y + 0.5f) / kTransmittanceHeight, r, mu);
                    const float length = distance_to_top(r, mu);
                    constexpr int kSteps = 400;
                    Vec3f depth {};
                    for (int k = 0; k < kSteps; k++)
                    {
                        const float t = (k + 0.5f) * length / kSteps;
                        const float rk = std::sqrt(r * r + t * t + 2 * r * mu * t);
                        depth = add(depth, scale(medium(a.settings, rk - kGround).extinction, length / kSteps));
                    }
                    a.transmittance[size_t(y) * kTransmittanceWidth + x] = exp_of(scale(depth, -1));
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

    Atmosphere make_atmosphere(const SkySettings &settings)
    {
        Atmosphere a;
        a.settings = settings;
        a.settings.aerosol_depth = std::min(std::max(settings.aerosol_depth, 0.0f), 2.0f);
        a.settings.ground_albedo = clamp01(settings.ground_albedo);
        a.settings.altitude_km = std::min(std::max(settings.altitude_km, 0.001f), 50.0f);
        make_transmittance(a);
        make_multiple(a);
        return a;
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

    SkyMap physical_sky_map(const Atmosphere &atmosphere, const Vec3f &toward_light, const Vec3f &light_shade, float min_elevation)
    {
        // The light's direction held at min_elevation or above, for the brightness's scale only.
        const float elevation = std::asin(std::min(std::max(toward_light[1], -1.0f), 1.0f));
        const float flat = std::sqrt(toward_light[0] * toward_light[0] + toward_light[2] * toward_light[2]);
        const float held = std::max(elevation, min_elevation);
        const Vec3f across = flat > 1e-6f ? Vec3f { toward_light[0] / flat, 0, toward_light[2] / flat } : Vec3f { 1, 0, 0 };
        const Vec3f reference = { across[0] * std::cos(held), std::sin(held), across[2] * std::cos(held) };
        const float direct = luminance_of(sky_transmittance(atmosphere, reference)) / kPi;
        const float k = direct > 1e-9f ? std::max(luminance_of(light_shade), 0.0f) / direct : 0.0f;

        SkyMap sky;
        sky.texels.assign(size_t(kSkyMapSide) * kSkyMapSide * 4, 0.0f);
        for (uint32_t y = 0; y < kSkyMapSide; y++)
            for (uint32_t x = 0; x < kSkyMapSide; x++)
            {
                Vec3f sum {};
                for (uint32_t s = 0; s < 4; s++)
                {
                    const Vec3f d = sky_direction((x + (s % 2 + 0.5f) / 2) / kSkyMapSide, (y + (s / 2 + 0.5f) / 2) / kSkyMapSide);
                    sum = add(sum, sky_radiance(atmosphere, d, toward_light));
                }
                float *texel = &sky.texels[(size_t(y) * kSkyMapSide + x) * 4];
                for (int c = 0; c < 3; c++)
                    texel[c] = sum[c] / 4 * k;
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

    Vec3f night_tint(float sky_sun_y)
    {
        // A blue of luminance 1.
        const Vec3f blue = { 0.848f, 1.007f, 1.379f };
        const float t = clamp01(-sky_sun_y / std::sin(6 * kPi / 180));
        return { 1 + (blue[0] - 1) * t, 1 + (blue[1] - 1) * t, 1 + (blue[2] - 1) * t };
    }

    void tint_sky(SkyMap &sky, const Vec3f &tint)
    {
        for (size_t i = 0; i + 3 < sky.texels.size(); i += 4)
            for (int c = 0; c < 3; c++)
                sky.texels[i + size_t(c)] *= tint[size_t(c)];
        for (Vec3f &h : sky.harmonics)
            h = mul(h, tint);
    }

    bool sky_request_differs(const SkyRequest &a, const SkyRequest &b)
    {
        const float la = luminance_of(a.shade), lb = luminance_of(b.shade);
        const float tint = std::max({ std::fabs(a.tint[0] - b.tint[0]), std::fabs(a.tint[1] - b.tint[1]), std::fabs(a.tint[2] - b.tint[2]) });
        return dot(a.toward_light, b.toward_light) < 0.9999905f || std::fabs(la - lb) > 0.01f * std::max(la, lb) || a.haze != b.haze || tint > 0.005f;
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
        std::optional<Atmosphere> air;
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
            if (!air || air->settings.aerosol_depth != std::min(std::max(request.haze, 0.0f), 2.0f))
            {
                SkySettings settings;
                settings.aerosol_depth = request.haze;
                air = make_atmosphere(settings);
            }
            auto result = std::make_shared<SkyResult>();
            result->request = request;
            result->map = physical_sky_map(*air, request.toward_light, request.shade);
            tint_sky(result->map, request.tint);
            result->horizon = sky_horizon(result->map);
            result->version = ++version;
            std::lock_guard<std::mutex> lock(mutex_);
            latest_ = std::move(result);
        }
    }
}
