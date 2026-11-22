// The physical sky's air on the GPU, as core/sky.cpp computes it on the CPU and from the same tables: Hillaire's
// atmosphere over a spherical planet, kilometers and coefficients per kilometer. Include after common.hlsli.

Texture2D<float4> sky_transmittance_table : register(t9);   // Atmosphere::transmittance, over Bruneton's (mu, r) mapping
Texture2D<float4> sky_multiple_table : register(t10);       // Atmosphere::multiple, over the light's cosine and the height

static const float kSkyPi = 3.14159265;
static const float kSkyGround = 6360, kSkyTop = 6460;
static const float3 kSkyRayleigh = float3(5.802e-3, 13.558e-3, 33.1e-3);
static const float kSkyRayleighHeight = 8;
static const float kSkyMieScatter = 3.996e-3, kSkyMieExtinction = 4.40e-3, kSkyMieHeight = 1.2, kSkyMieG = 0.8;
static const float3 kSkyOzone = float3(1.85e-3, 1.881e-3, 0.085e-3);
static const float kSkyOzoneCenter = 25, kSkyOzoneHalfWidth = 15;
static const float kSkyGroundAlbedo = 0.3;
// The sky view tables' size and their layout: the world's azimuth across, the elevation down with its square root, so
// rows crowd toward the horizon.
static const float2 kSkyViewSize = float2(256, 128);

struct SkyMedium
{
    float3 rayleigh;
    float mie;
    float3 extinction;
};

SkyMedium sky_medium(float height)
{
    float r = exp(-height / kSkyRayleighHeight), m = exp(-height / kSkyMieHeight) * sky_haze / (kSkyMieExtinction * kSkyMieHeight);
    float o = max(0, 1 - abs(height - kSkyOzoneCenter) / kSkyOzoneHalfWidth);
    SkyMedium medium;
    medium.rayleigh = kSkyRayleigh * r;
    medium.mie = kSkyMieScatter * m;
    medium.extinction = medium.rayleigh + kSkyMieExtinction * m + kSkyOzone * o;
    return medium;
}

float sky_rayleigh_phase(float c) { return 3 / (16 * kSkyPi) * (1 + c * c); }

float sky_mie_phase(float c)
{
    float g2 = kSkyMieG * kSkyMieG;
    return 3 / (8 * kSkyPi) * (1 - g2) * (1 + c * c) / ((2 + g2) * pow(max(1 + g2 - 2 * kSkyMieG * c, 1e-6), 1.5));
}

float sky_distance_to_top(float r, float mu) { return -r * mu + sqrt(max(r * r * (mu * mu - 1) + kSkyTop * kSkyTop, 0)); }

float sky_distance_to_ground(float r, float mu)
{
    float d = r * r * (mu * mu - 1) + kSkyGround * kSkyGround;
    return mu < 0 && d >= 0 ? -r * mu - sqrt(d) : -1;
}

float sky_light_visible(float r, float mu)
{
    float horizon = -sqrt(max(1 - (kSkyGround / r) * (kSkyGround / r), 0));
    float t = saturate((mu - horizon + 0.005) / 0.01);
    return t * t * (3 - 2 * t);
}

float3 sky_transmittance_to_top(float r, float mu)
{
    float h = sqrt(kSkyTop * kSkyTop - kSkyGround * kSkyGround), rho = sqrt(max(r * r - kSkyGround * kSkyGround, 0));
    float d = sky_distance_to_top(r, mu), d_min = kSkyTop - r, d_max = rho + h;
    float2 uv = float2((d - d_min) / max(d_max - d_min, 1e-6), rho / h);
    return sky_transmittance_table.SampleLevel(linear_clamp, uv, 0).rgb;
}

// The light of unit illuminance above the air that reaches radius r with cosine mu, the planet's shadow included.
float3 sky_light_at(float r, float mu) { return sky_transmittance_to_top(r, mu) * sky_light_visible(r, mu); }

float3 sky_multiple_at(float r, float mu)
{
    return sky_multiple_table.SampleLevel(linear_clamp, float2(mu * 0.5 + 0.5, (r - kSkyGround) / (kSkyTop - kSkyGround)), 0).rgb;
}

float3 sky_step(float3 inscatter, float3 extinction, float dt)
{
    return extinction > 1e-9 ? inscatter * (1 - exp(-extinction * dt)) / extinction : inscatter * dt;
}

float sky_viewer_radius() { return kSkyGround + sky_camera_km; }

// What of a light above the air reaches the viewer along view.
float3 sky_view_transmittance(float3 view) { return sky_light_at(sky_viewer_radius(), view.y); }

float2 sky_view_uv(float3 view)
{
    float azimuth = atan2(view.z, view.x), elevation = asin(clamp(view.y, -1, 1));
    float s = sign(elevation) * sqrt(abs(elevation) / (kSkyPi / 2));
    // Held inside the first and last rows' centers: the wrapping sampler must not blend the zenith with the nadir.
    float v = clamp((1 - s) * 0.5, 0.5 / kSkyViewSize.y, 1 - 0.5 / kSkyViewSize.y);
    return float2(azimuth / (2 * kSkyPi) + 0.5, v);
}

float3 sky_view_direction(float2 uv)
{
    float s = 1 - 2 * uv.y, elevation = sign(s) * s * s * (kSkyPi / 2), azimuth = (uv.x - 0.5) * 2 * kSkyPi;
    return float3(cos(elevation) * cos(azimuth), sin(elevation), cos(elevation) * sin(azimuth));
}

// core/sky.cpp's night_glow: airglow seen edge on toward the horizon, and the background of faint stars and zodiacal light,
// through the air. A Milky Way would add here.
float3 sky_night_glow(float3 view)
{
    if (view.y <= 0)
        return 0;
    const float kZenith = 1.35e-9, kAirglow = 0.6;
    const float3 kAirglowColor = float3(0.80, 1.0, 1.45);
    float ratio = kSkyGround / (kSkyGround + 90), edge_on = rsqrt(max(1 - ratio * ratio * (1 - view.y * view.y), 1e-4));
    return (kAirglowColor * (kZenith * kAirglow * edge_on) + kZenith * (1 - kAirglow)) * sky_view_transmittance(view);
}

// The optical depth over km kilometers along a ray of cosine mu from the viewer's height, of a layer of scale height H, over
// its density at the ground.
float layer_depth(float km, float mu, float H)
{
    float rise = mu * km;
    return exp(-sky_camera_km / H) * (abs(rise) > 1e-4 * H ? H * (1 - exp(-rise / H)) / mu : km);
}

// The air over km kilometers along view: what of the light behind it comes through, and the light it scatters toward the
// viewer, taken at its middle height: the sun's and the moon's light that reaches it (the planet's shadow included)
// scattered once at each phase, lit of it where no terrain shades it, and the light scattered again. The sky pass gives
// the scene's points it, and the clouds their light-weighted depth (clouds_ps.hlsl).
float3 aerial_light(float km, float3 view, float lit, out float3 through)
{
    float mu = view.y;
    float3 thickness = kSkyRayleigh * layer_depth(km, mu, kSkyRayleighHeight) + (sky_haze / kSkyMieHeight) * layer_depth(km, mu, kSkyMieHeight);
    through = exp(-thickness);
    float height = max(sky_camera_km + mu * km * 0.5, 0), r = kSkyGround + height;
    SkyMedium m = sky_medium(height);
    float cs = dot(view, sky_toward_sun), cm = dot(view, sky_toward_moon);
    float3 sun = sky_light_at(r, sky_toward_sun.y), moon = sky_light_at(r, sky_toward_moon.y) * sky_moon_illuminance;
    float3 once = sun * (m.rayleigh * sky_rayleigh_phase(cs) + m.mie * sky_mie_phase(cs))
        + moon * (m.rayleigh * sky_rayleigh_phase(cm) + m.mie * sky_mie_phase(cm));
    float3 again = (m.rayleigh + m.mie) * (sky_multiple_at(r, sky_toward_sun.y) + sky_multiple_at(r, sky_toward_moon.y) * sky_moon_illuminance);
    // Under an overcast the clouds take most of the sunlight the air would scatter, and the rest grays.
    float3 inscatter = (lit * once * (1 - 0.9 * sky_overcast) + again * (1 - 0.5 * sky_overcast)) * (1 - through) / max(m.extinction, 1e-6);
    inscatter = lerp(inscatter, dot(inscatter, float3(0.2126, 0.7152, 0.0722)), sky_overcast);
    return inscatter * sky_scale;
}
