// The physical sky along a pixel: the air's light from sky_view_ps.hlsl's tables with each light's phase functions at
// the pixel's own angle, the night's glow, the sun's limb-darkened disc and the moon's lit face, all through the air, at
// the sky's scale. For sky_ps.hlsl and sky_dome_ps.hlsl, after common.hlsli and sky.hlsli.
Texture2D<float4> view_sun_rayleigh : register(t1);
Texture2D<float4> view_sun_mie : register(t3);
Texture2D<float4> view_moon_rayleigh : register(t4);
Texture2D<float4> view_moon_mie : register(t5);
Texture2D<float4> view_rest : register(t6);
Texture2D<float4> moon_face : register(t7);         // the near side: rgb its color over its luminance, halved; a its albedo over
                                                    // its mean, over 3, gamma 2.2
SamplerState linear_wrap : register(s3);
Texture2D<float> auto_ev : register(t14);           // the auto exposure's last stops; unbound, 0, while it is off

// The most a full moon's face shows on average after the exposure, below the tone map's white: the real face is some 11
// stops over the ground it lights and would show as a white disc.
static const float kMoonFaceShown = 1.4;

// The sun's radius and its limb darkening per channel, a linear law at about 680, 550 and 440 nm.
static const float kSunRadius = 0.004654;
static const float3 kSunLimb = float3(0.47, 0.58, 0.70);
static const float kMoonRadius = 0.004528 * 20;   // the drawn moon: core/sky.h's kMoonRadius times kMoonDrawn

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
    float shown = sky_moon_face * exp2(auto_ev.Load(int3(0, 0, 0)));
    float dim = shown > kMoonFaceShown ? kMoonFaceShown / shown : 1;
    return (sky_moon_radiance * shading + sky_earthshine) * albedo * color * coverage * dim;
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

// The brightest the HDR scene's half floats hold, with room for the game's own passes to add.
static const float kMostLight = 60000;

// The sky at pixel position.
float3 drawn_sky(float2 position)
{
    float3 near = view_position(position * render_size.zw, 1);
    float3 view = normalize(mul(normalize(near), (float3x3)world_to_view));
    float pixel_angle = max(length(ddx(view)), length(ddy(view)));
    // Below the horizon the tables hold the planet's dark ground, which the game's world covers; where the world ends short
    // of the horizon, as past a map's edge seen from high up, the horizon's haze carries on down instead.
    float r = sky_viewer_radius();
    float lowest = -sqrt(max(1 - (kSkyGround / r) * (kSkyGround / r), 0)) + 0.002;
    float3 air_view = view.y < lowest ? float3(normalize(view.xz + 1e-6) * sqrt(1 - lowest * lowest), lowest).xzy : view;
    float3 light = air_light(air_view) + sky_night_glow(air_view);
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
