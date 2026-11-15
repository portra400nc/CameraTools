// The physical sky over the sky's pixels (depth 0): the air's light from sky_view_ps.hlsl's tables with each light's
// phase functions at the pixel's own angle, the night's glow, the sun's limb-darkened disc and the moon's lit face, all
// through the air, at the sky's scale; and over the scene's pixels the air between them and the viewer. Writes every
// pixel, because the result is copied over the game's HDR scene whole.
#include "common.hlsli"
#include "sky.hlsli"

Texture2D<float3> scene : register(t0);             // mirror of the game's HDR scene
Texture2D<float4> view_sun_rayleigh : register(t1);
Texture2D<float> depth : register(t2);              // mirror of the main depth, reversed Z
Texture2D<float4> view_sun_mie : register(t3);
Texture2D<float4> view_moon_rayleigh : register(t4);
Texture2D<float4> view_moon_mie : register(t5);
Texture2D<float4> view_rest : register(t6);
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

// Aerial perspective: the scene point's light through the air between, sky_aerial times as far as it is, and the air's
// own light along the way. The air's light toward a direction is the sky's there, a share of it as the segment holds a
// share of that direction's whole air; looking down, the horizon's. The night's glow shines from 90 km up, above it all.
float3 aerial(float3 color, float meters, float3 view)
{
    float km = meters * 0.001 * sky_aerial, mu = view.y;
    float3 thickness = kSkyRayleigh * layer_depth(km, mu, kSkyRayleighHeight) + (sky_haze / kSkyMieHeight) * layer_depth(km, mu, kSkyMieHeight);
    float3 through = exp(-thickness);
    float3 level = normalize(float3(view.x, max(view.y, 0.02), view.z));
    float3 whole = 1 - sky_view_transmittance(level);
    float3 share = saturate((1 - through) / max(whole, 1e-3));
    return color * through + air_light(level) * share * sky_scale;
}

float3 main(float4 position : SV_Position) : SV_Target0
{
    int2 pixel = int2(position.xy);
    float3 color = scene.Load(int3(pixel, 0));
    float scene_depth = depth.Load(int3(pixel, 0));
    if (scene_depth > 0)
    {
        if (sky_aerial <= 0)
            return color;
        float3 at = view_position(position.xy * render_size.zw, scene_depth);
        float3 toward = normalize(mul(normalize(at), (float3x3)world_to_view));
        return min(aerial(color, length(at), toward), kMostLight);
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
