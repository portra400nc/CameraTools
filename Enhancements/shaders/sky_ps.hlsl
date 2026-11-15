// The physical sky over the sky's pixels (depth 0): the air's light from sky_view_ps.hlsl's tables with each light's
// phase functions at the pixel's own angle, the night's glow, the sun's limb-darkened disc and the moon's lit face, all
// through the air, at the sky's scale; and over the scene's pixels the air between them and the viewer. Writes every
// pixel, because the result is copied over the game's HDR scene whole.
#include "common.hlsli"
#include "sky.hlsli"
#include "vegetation.hlsli"

Texture2D<float3> scene : register(t0);             // mirror of the game's HDR scene
Texture2D<float4> view_sun_rayleigh : register(t1);
Texture2D<float> depth : register(t2);              // mirror of the main depth, reversed Z
Texture2D<float4> view_sun_mie : register(t3);
Texture2D<float4> view_moon_rayleigh : register(t4);
Texture2D<float4> view_moon_mie : register(t5);
Texture2D<float4> view_rest : register(t6);
Texture2D<float> aerial_lit : register(t8);         // aerial_lit_ps.hlsl's share of lit air, half size, while sky_aerial_lit
Texture2D<float4> moon_face : register(t7);         // the near side: rgb its color over its luminance, halved; a its albedo over
                                                    // its mean, over 3, gamma 2.2
SamplerState linear_wrap : register(s3);

// The sun's radius and its limb darkening per channel, a linear law at about 680, 550 and 440 nm.
static const float kSunRadius = 0.004654;
static const float3 kSunLimb = float3(0.47, 0.58, 0.70);
static const float kMoonRadius = 0.004528;
// The brightest the HDR scene's half floats hold, with room for the game's own passes to add.
static const float kMostLight = 60000;

// The angle between two unit vectors, accurate when they are close.
float angle_between(float3 a, float3 b) { return 2 * asin(saturate(length(a - b) * 0.5)); }

float3 moon_light(float3 view, float pixel)
{
    float3 m = sky_toward_moon;
    float apart = angle_between(view, m);
    if (apart > kMoonRadius + 2 * pixel)
        return 0;
    float3 north = normalize(sky_moon_north - dot(sky_moon_north, m) * m);
    // Unity's world is left-handed: right is up x forward.
    float3 right = cross(north, m);
    float2 at = float2(dot(view - m, right), dot(view - m, north)) / sin(kMoonRadius);
    float coverage = saturate((kMoonRadius - apart) / pixel + 0.5);
    float along = length(at);
    at *= along > 0.999 ? 0.999 / along : 1;
    float facing = sqrt(saturate(1 - dot(at, at)));
    float3 normal = at.x * right + at.y * north - facing * m;
    // The near side's longitude grows to the right as the earth sees it with north up.
    float longitude = atan2(at.x, facing), latitude = asin(clamp(at.y, -1, 1));
    float4 face = moon_face.SampleLevel(linear_clamp, float2(longitude / kSkyPi + 0.5, 0.5 - latitude / kSkyPi), 0);
    float albedo = pow(face.a, 2.2) * 3;
    float3 color = face.rgb * 2;
    float lit = dot(normal, sky_toward_sun);
    float shading = lit > 0 ? lit / (lit + facing) : 0;
    return (sky_moon_radiance * shading + sky_earthshine) * albedo * color * coverage;
}

// The air's light along view: the tables' with each light's phase at this angle.
float3 air_light(float3 view)
{
    float2 uv = sky_view_uv(view);
    float cs = dot(view, sky_toward_sun), cm = dot(view, sky_toward_moon);
    return view_rest.SampleLevel(linear_wrap, uv, 0).rgb
        + view_sun_rayleigh.SampleLevel(linear_wrap, uv, 0).rgb * sky_rayleigh_phase(cs) + view_sun_mie.SampleLevel(linear_wrap, uv, 0).rgb * sky_mie_phase(cs)
        + view_moon_rayleigh.SampleLevel(linear_wrap, uv, 0).rgb * sky_rayleigh_phase(cm) + view_moon_mie.SampleLevel(linear_wrap, uv, 0).rgb * sky_mie_phase(cm);
}

// The optical depth over km kilometers along a ray of cosine mu from the viewer's height, of a layer of scale height H, over
// its density at the ground.
float layer_depth(float km, float mu, float H)
{
    float rise = mu * km;
    return exp(-sky_camera_km / H) * (abs(rise) > 1e-4 * H ? H * (1 - exp(-rise / H)) / mu : km);
}

// Aerial perspective: the scene point's light through the air between, sky_aerial times as far as it is, and the light
// that air scatters toward the viewer, taken at its middle height: the sun's and the moon's light that reaches it (the
// planet's shadow included) scattered once at each phase, lit of it where no terrain shades it, and the light scattered
// again.
float3 aerial(float3 color, float meters, float3 view, float lit)
{
    float km = meters * 0.001 * sky_aerial, mu = view.y;
    float3 thickness = kSkyRayleigh * layer_depth(km, mu, kSkyRayleighHeight) + (sky_haze / kSkyMieHeight) * layer_depth(km, mu, kSkyMieHeight);
    float3 through = exp(-thickness);
    float height = max(sky_camera_km + mu * km * 0.5, 0), r = kSkyGround + height;
    SkyMedium m = sky_medium(height);
    float cs = dot(view, sky_toward_sun), cm = dot(view, sky_toward_moon);
    float3 sun = sky_light_at(r, sky_toward_sun.y), moon = sky_light_at(r, sky_toward_moon.y) * sky_moon_illuminance;
    float3 once = sun * (m.rayleigh * sky_rayleigh_phase(cs) + m.mie * sky_mie_phase(cs))
        + moon * (m.rayleigh * sky_rayleigh_phase(cm) + m.mie * sky_mie_phase(cm));
    float3 again = (m.rayleigh + m.mie) * (sky_multiple_at(r, sky_toward_sun.y) + sky_multiple_at(r, sky_toward_moon.y) * sky_moon_illuminance);
    float3 inscatter = (lit * once + again) * (1 - through) / max(m.extinction, 1e-6);
    return color * through + inscatter * sky_scale;
}

float3 main(float4 position : SV_Position) : SV_Target0
{
    int2 pixel = int2(position.xy);
    float3 color = scene.Load(int3(pixel, 0));
    float scene_depth = depth.Load(int3(pixel, 0));
    if (scene_depth > 0)
    {
        float3 at = view_position(position.xy * render_size.zw, scene_depth);
        color = night_vegetation(color, pixel, at);
        if (sky_aerial <= 0)
            return color;
        float3 toward = normalize(mul(normalize(at), (float3x3)world_to_view));
        float lit = sky_aerial_lit > 0.5 ? aerial_lit.Load(int3(pixel / 2, 0)) : 1;
        return min(aerial(color, length(at), toward, lit), kMostLight);
    }
    float3 near = view_position(position.xy * render_size.zw, 1);
    float3 view = normalize(mul(normalize(near), (float3x3)world_to_view));
    float pixel_angle = max(length(ddx(view)), length(ddy(view)));
    float3 light = air_light(view) + sky_night_glow(view);
    float3 through = sky_view_transmittance(view);
    float sun_apart = angle_between(view, sky_toward_sun);
    if (sun_apart < kSunRadius + 2 * pixel_angle)
    {
        float mu = sqrt(saturate(1 - (sun_apart / kSunRadius) * (sun_apart / kSunRadius)));
        float coverage = saturate((kSunRadius - sun_apart) / pixel_angle + 0.5);
        light += through * (1 - kSunLimb * (1 - mu)) / ((1 - kSunLimb / 3) * kSkyPi * kSunRadius * kSunRadius) * coverage;
    }
    light += through * moon_light(view, pixel_angle);
    return min(light * sky_scale, kMostLight);
}
