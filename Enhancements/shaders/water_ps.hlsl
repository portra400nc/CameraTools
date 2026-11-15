// Physical water, drawn in place of the game's water pixel shaders with their geometry, so only the water's light is
// ours: the bed seen through the water, dimmed by its depth and filled with the water's own color as Clarity sets it,
// and, by Fresnel's law, what the surface mirrors on small waves: the scene in front of it where the frame shows it,
// else the physical sky, and the sun's and the moon's glints. Everything is in the physical sky's units, so the water
// darkens with the night as the sky does.
#include "common.hlsli"
#include "sky.hlsli"

Texture2D<float> game_depth : register(t0);         // the game's own copy of the opaque scene's depth, which its water reads
Texture2D<float3> scene : register(t16);            // the HDR scene as the first water draw found it
Texture2D<float4> view_sun_rayleigh : register(t17);
Texture2D<float4> view_sun_mie : register(t18);
Texture2D<float4> view_moon_rayleigh : register(t19);
Texture2D<float4> view_moon_mie : register(t20);
Texture2D<float4> view_rest : register(t21);
SamplerState linear_wrap : register(s3);

static const float kWaterF0 = 0.02;                 // Fresnel at normal incidence, water against air
static const uint kWaves = 8;
static const uint kReflectSteps = 32;

float3 air_light(float3 view)
{
    float2 uv = sky_view_uv(view);
    float cs = dot(view, sky_toward_sun), cm = dot(view, sky_toward_moon);
    return view_rest.SampleLevel(linear_wrap, uv, 0).rgb
        + view_sun_rayleigh.SampleLevel(linear_wrap, uv, 0).rgb * sky_rayleigh_phase(cs) + view_sun_mie.SampleLevel(linear_wrap, uv, 0).rgb * sky_mie_phase(cs)
        + view_moon_rayleigh.SampleLevel(linear_wrap, uv, 0).rgb * sky_rayleigh_phase(cm) + view_moon_mie.SampleLevel(linear_wrap, uv, 0).rgb * sky_mie_phase(cm);
}

// The surface's slope at world (x, z): deep-water waves from a quarter meter to five meters long, spread over directions
// by the golden angle, each as steep as its length allows, moving at its own speed.
float2 wave_slope(float2 p)
{
    float2 slope = 0;
    [unroll] for (uint i = 0; i < kWaves; i++)
    {
        float angle = i * 2.39996323, length_m = 0.25 * pow(1.55, i);
        float2 d = float2(cos(angle), sin(angle));
        float k = 6.2831853 / length_m, speed = sqrt(9.81 * k);
        slope += d * (0.012 * length_m * k) * cos(k * dot(d, p) - speed * water_time);
    }
    return slope * water_waves / kWaves;
}

// A light's glint off the waves: GGX with the roughness of the ripples too small to draw, Fresnel at the half vector.
float3 glint(float3 n, float3 v, float3 l, float3 light)
{
    float nl = dot(n, l), nv = max(dot(n, v), 0.05);
    if (nl <= 0)
        return 0;
    float3 h = normalize(l + v);
    float a = 0.01 + 0.04 * water_waves, a2 = a * a, nh = saturate(dot(n, h));
    float d = a2 / (kSkyPi * pow(nh * nh * (a2 - 1) + 1, 2));
    float f = kWaterF0 + (1 - kWaterF0) * pow(1 - saturate(dot(v, h)), 5);
    return light * d * f * nl / (4 * nv);
}

// The scene the reflected ray meets in the frame, in view space from p along r, or a = 0 where it leaves the frame,
// meets the sky or goes behind what it would have to pass.
float4 reflected_scene(float3 p, float3 r)
{
    float t = 0.15;
    [loop] for (uint i = 0; i < kReflectSteps; i++)
    {
        t *= 1.18;
        float3 q = p + r * t;
        if (q.z > -0.05)
            return 0;
        float2 uv = stored_of(q);
        if (any(uv < 0) || any(uv > 1))
            return 0;
        int2 pixel = int2(uv * render_size.xy);
        float d = game_depth.Load(int3(pixel, 0));
        if (d <= 0)
            continue;
        float surface = view_position(uv, d).z;
        float behind = surface - q.z;
        if (behind > 0)
        {
            // Within the step's own length the ray met the surface; farther behind, it passed under something.
            if (behind > t * 0.25 + 0.5)
                return 0;
            float2 edge = saturate(min(uv, 1 - uv) * 12);
            return float4(scene.Load(int3(pixel, 0)), edge.x * edge.y);
        }
    }
    return 0;
}

float4 main(float4 position : SV_Position) : SV_Target0
{
    int2 pixel = int2(position.xy);
    float2 uv = position.xy * render_size.zw;
    float3 at = view_position(uv, position.z);
    float3x3 to_view = (float3x3)world_to_view;
    float3 eye_offset = float3(world_to_view[0].w, world_to_view[1].w, world_to_view[2].w);
    float3 world = mul(transpose(to_view), at - eye_offset);
    float2 slope = wave_slope(world.xz);
    float3 n_world = normalize(float3(-slope.x, 1, -slope.y));
    float3 n = normalize(mul(to_view, n_world));
    float3 v = -normalize(at);
    float3 v_world = mul(transpose(to_view), v);
    float nv = saturate(dot(n, v));
    float fresnel = kWaterF0 + (1 - kWaterF0) * pow(1 - nv, 5);

    // What the surface mirrors: the frame, else the sky.
    float3 r = reflect(-v, n);
    float3 r_world = mul(transpose(to_view), r);
    float3 sky = air_light(normalize(float3(r_world.x, max(r_world.y, 0.01), r_world.z))) * sky_scale;
    float4 met = reflected_scene(at, r);
    float3 mirrored = lerp(sky, met.rgb, met.a);

    // The bed through the water: the opaque scene behind, bent a little by the waves, dimmed by the water between.
    float bed_depth = game_depth.Load(int3(pixel, 0));
    float path = bed_depth > 0 ? max(length(view_position(uv, bed_depth)) - length(at), 0) : 100;
    int2 bent = clamp(pixel + int2(n.xy * min(path, 3) * render_size.y * 0.01), 0, int2(render_size.xy) - 1);
    float bent_depth = game_depth.Load(int3(bent, 0));
    int2 seen = bent_depth > 0 && bent_depth < position.z ? bent : pixel;
    float3 absorb = float3(0.45, 0.07, 0.035) * (6 / water_clarity) + 0.01;
    float3 through = exp(-absorb * path);
    // The water's own color: the sky's and the sun's light scattered back by the water it lies in.
    float3 lit_by = (air_light(float3(0, 1, 0)) * kSkyPi + sky_light_at(sky_viewer_radius(), sky_toward_sun.y) * max(sky_toward_sun.y, 0)
        + sky_light_at(sky_viewer_radius(), sky_toward_moon.y) * sky_moon_illuminance * max(sky_toward_moon.y, 0)) * sky_scale;
    float3 body = lit_by * float3(0.004, 0.012, 0.014) * (1 - through);
    float3 seen_through = scene.Load(int3(seen, 0)) * through + body;

    float3 sun = sky_light_at(sky_viewer_radius(), sky_toward_sun.y) * sky_scale;
    float3 moon = sky_light_at(sky_viewer_radius(), sky_toward_moon.y) * sky_moon_illuminance * sky_scale;
    float3 glints = glint(n_world, v_world, sky_toward_sun, sun) + glint(n_world, v_world, sky_toward_moon, moon);
    return float4(min(lerp(seen_through, mirrored, fresnel) + glints, 60000), 1);
}
