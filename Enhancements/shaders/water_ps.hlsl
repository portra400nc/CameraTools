// Physical water, drawn in place of the game's water pixel shaders with their geometry, so only the water's light is
// ours, added to the target as the change it makes to the opaque scene: the bed, dimmed by the water's depth and filled
// with the water's own color as Clarity sets it,
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
static const uint kWaveLayers = 5;
static const uint kReflectSteps = 48;

float3 air_light(float3 view)
{
    float2 uv = sky_view_uv(view);
    float cs = dot(view, sky_toward_sun), cm = dot(view, sky_toward_moon);
    return view_rest.SampleLevel(linear_wrap, uv, 0).rgb
        + view_sun_rayleigh.SampleLevel(linear_wrap, uv, 0).rgb * sky_rayleigh_phase(cs) + view_sun_mie.SampleLevel(linear_wrap, uv, 0).rgb * sky_mie_phase(cs)
        + view_moon_rayleigh.SampleLevel(linear_wrap, uv, 0).rgb * sky_rayleigh_phase(cm) + view_moon_mie.SampleLevel(linear_wrap, uv, 0).rgb * sky_mie_phase(cm);
}

float hash3(float3 p)
{
    p = frac(p * 0.1031);
    p += dot(p, p.zyx + 31.32);
    return frac((p.x + p.y) * p.z);
}

// Value noise over (x, z, time), smooth, from -1 to 1.
float noise3(float3 p)
{
    float3 i = floor(p), f = p - i;
    f = f * f * (3 - 2 * f);
    float a = lerp(lerp(hash3(i), hash3(i + float3(1, 0, 0)), f.x), lerp(hash3(i + float3(0, 1, 0)), hash3(i + float3(1, 1, 0)), f.x), f.y);
    float b = lerp(lerp(hash3(i + float3(0, 0, 1)), hash3(i + float3(1, 0, 1)), f.x), lerp(hash3(i + float3(0, 1, 1)), hash3(i + float3(1, 1, 1)), f.x), f.y);
    return lerp(a, b, f.z) * 2 - 1;
}

// The water's height at world (x, z), in meters: five layers of noise from six meters long down to forty centimeters,
// each turned its own way and carried down the wind at the speed deep water carries waves of its length, which also
// sets how fast each one changes, and two long swells along the wind. water_waves scales its height, water_size its length. A layer fades out where
// its waves span fewer than eight of pixel_m, the meters a pixel covers there, so far water does not shimmer; no layer
// shorter than shortest counts.
float wave_height(float2 world, float pixel_m, float shortest)
{
    // Wave size stretches the waves and their height alike, and slows them as longer waves are slower.
    float2 p = world / water_size;
    pixel_m /= water_size;
    shortest /= water_size;
    float2 wind = float2(cos(water_wind), sin(water_wind));
    float h = 0;
    float length_m = 6, amplitude = 0.12;
    [unroll] for (uint i = 0; i < kWaveLayers; i++)
    {
        float angle = water_wind + (i * 2.39996323 - 1.2) * 0.35;
        float2x2 turn = float2x2(cos(angle), -sin(angle), sin(angle), cos(angle));
        float speed = sqrt(9.81 * length_m / 6.2831853);
        float2 q = mul(turn, p - wind * speed * water_time) / length_m;
        float seen = saturate(length_m / (8 * pixel_m) - 1) * (length_m >= shortest ? 1 : 0);
        h += seen * amplitude * noise3(float3(q, water_time * speed / length_m * 0.5 + i * 17.13));
        length_m *= 0.53;
        amplitude *= 0.5;
    }
    [unroll] for (uint k = 0; k < 2; k++)
    {
        float angle = water_wind + (k == 0 ? 0.3 : -0.45), swell = k == 0 ? 11 : 7.3;
        float2 d = float2(cos(angle), sin(angle));
        float kw = 6.2831853 / swell;
        // Faded as the layers are: unfaded, far out the swells aliased into rings around the camera.
        float seen = saturate(swell / (8 * pixel_m) - 1);
        h += seen * 0.05 * sin(kw * dot(d, p) - sqrt(9.81 * kw) * water_time + k * 2.1);
    }
    return h * water_waves * water_size;
}

// The surface's slope at world (x, z), from the heights a few centimeters apart.
float2 wave_slope(float2 p, float pixel_m, float shortest)
{
    const float e = 0.04;
    float h = wave_height(p, pixel_m, shortest);
    return float2(wave_height(p + float2(e, 0), pixel_m, shortest) - h, wave_height(p + float2(0, e), pixel_m, shortest) - h) / e;
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

// How far behind the frame's surface view space point q lies, in meters, and that surface's pixel: below 0 in front of
// it, and in front of nothing over the sky. off: q is off the frame or behind the camera.
float behind_frame(float3 q, out int2 pixel, out bool off)
{
    float2 uv = stored_of(q);
    pixel = int2(uv * render_size.xy);
    off = q.z > -0.05 || any(uv < 0) || any(uv > 1);
    float d = off ? 0 : game_depth.Load(int3(pixel, 0));
    return d > 0 ? view_position(uv, d).z - q.z : -1e30;
}

// The scene the reflected ray meets in the frame, in view space from p along r, or a = 0 where it leaves the frame or
// meets the sky. Where it crosses behind a surface, the crossing is found by halving the step, and taken when the ray
// passed close to the surface there; where it went under something, as through a gap in leaves, it marches on.
float4 reflected_scene(float3 p, float3 r)
{
    float t = 0.1, last = 0;
    int2 pixel;
    bool off;
    [loop] for (uint i = 0; i < kReflectSteps; i++)
    {
        last = t;
        t *= 1.13;
        float behind = behind_frame(p + r * t, pixel, off);
        if (off)
            return 0;
        if (behind <= 0)
            continue;
        float lo = last, hi = t;
        [unroll] for (uint k = 0; k < 5; k++)
        {
            float mid = 0.5 * (lo + hi);
            if (behind_frame(p + r * mid, pixel, off) > 0)
                hi = mid;
            else
                lo = mid;
        }
        // How sure the crossing is: fully where the ray met the surface within a tolerance, fading over a second one.
        float tolerance = max(0.3, 0.04 * hi), miss = behind_frame(p + r * hi, pixel, off);
        float sure = saturate(2 - miss / tolerance);
        if (sure > 0)
        {
            float2 uv = stored_of(p + r * hi);
            float2 edge = saturate(min(uv, 1 - uv) * 12);
            return float4(scene.Load(int3(pixel, 0)), edge.x * edge.y * sure);
        }
    }
    return 0;
}

RWTexture2D<uint> water_marks : register(u7);  // the frame (top 8 bits) and depth (low 24) of the water each pixel took

// Early depth, so a mesh's water hidden behind the scene leaves no mark.
[earlydepthstencil]
float4 main(float4 position : SV_Position) : SV_Target0
{
    int2 pixel = int2(position.xy);
    // A pixel takes a surface's water once a frame: where two of the game's water meshes overlap, the second one's would
    // add the change again. Another surface, one at another depth, still takes its own.
    uint depth_bits = uint(saturate(position.z) * 16777215.0);
    uint before;
    InterlockedExchange(water_marks[pixel], (water_stamp << 24) | depth_bits, before);
    float before_depth = float(before & 0xFFFFFF);
    if ((before >> 24) == water_stamp && abs(before_depth - float(depth_bits)) <= max(0.01 * float(depth_bits), 16.0))
        discard;
    float2 uv = position.xy * render_size.zw;
    float3 at = view_position(uv, position.z);
    float3x3 to_view = (float3x3)world_to_view;
    float3 eye_offset = float3(world_to_view[0].w, world_to_view[1].w, world_to_view[2].w);
    float3 world = mul(transpose(to_view), at - eye_offset);
    // The meters a pixel covers on the water here, along the view.
    float pixel_m = length(at) / max(projection_scale, 1);
    float2 slope = wave_slope(world.xz, pixel_m, 0);
    float3 n_world = normalize(float3(-slope.x, 1, -slope.y));
    float3 n = normalize(mul(to_view, n_world));
    // The mirrored ray follows only the waves a meter and longer: the small ripples' reflections would change from
    // pixel to pixel and frame to frame.
    float2 swell = wave_slope(world.xz, pixel_m, 1);
    float3 n_mirror = normalize(mul(to_view, normalize(float3(-swell.x, 1, -swell.y))));
    float3 v = -normalize(at);
    float3 v_world = mul(transpose(to_view), v);
    float nv = saturate(dot(n, v));
    float fresnel = kWaterF0 + (1 - kWaterF0) * pow(1 - nv, 5);

    // What the surface mirrors: the frame, else the sky. At a low view a long wave tilts the mirrored ray down into the
    // water, where it met the bed through the frame and flickered as the wave passed; a slope that steep hides behind
    // the next wave, so the ray rises at least half as steeply as off still water.
    float3 r_world = mul(transpose(to_view), reflect(-v, n_mirror));
    r_world.y = max(r_world.y, 0.5 * max(v_world.y, 0));
    r_world = normalize(r_world);
    float3 r = mul(to_view, r_world);
    float3 sky = air_light(normalize(float3(r_world.x, max(r_world.y, 0.01), r_world.z))) * sky_scale;
    float4 met = reflected_scene(at, r);
    float3 mirrored = lerp(sky, met.rgb, met.a);

    // The bed through the water: the opaque scene behind, dimmed by the water between.
    float bed_depth = game_depth.Load(int3(pixel, 0));
    float path = bed_depth > 0 ? max(length(view_position(uv, bed_depth)) - length(at), 0) : 100;
    // Far out the depth buffer cannot part the bed from the surface, and the water read a few centimeters deep in bands,
    // showing the sunlit bed through it; past a hundred meters the water is taken to be at least a twentieth as deep as
    // it is far, which a sea's floor is at such a slant anyway.
    path = max(path, max(length(at) - 100, 0) * 0.05);
    // Near the end of the seabed the game draws, the water deepens over some 120 m of the surface instead of at once: past
    // it the depth buffer holds no bed, and the shallows' sunlit sand ended at a hard line against deep water.
    if (bed_depth > 0)
    {
        float reach = clamp(120 / max(pixel_m, 1e-3), 4, 160);
        float bedded = 0;
        [unroll] for (uint k = 0; k < 18; k++)
        {
            float angle = k * 2.3999632, radius = reach * (k % 3 + 1) / 3;
            int2 q = clamp(pixel + int2(float2(cos(angle), sin(angle)) * radius), int2(0, 0), int2(render_size.xy) - 1);
            bedded += game_depth.Load(int3(q, 0)) > 0 ? 1 : 0;
        }
        path = lerp(max(path, 100), path, smoothstep(0.3, 1, bedded / 18));
    }
    float3 absorb = float3(0.45, 0.07, 0.035) * (6 / water_clarity) + 0.01;
    float3 through = exp(-absorb * path);
    // The water's own color: the sky's and the sun's light scattered back by the water it lies in.
    float3 lit_by = (air_light(float3(0, 1, 0)) * kSkyPi + sky_light_at(sky_viewer_radius(), sky_toward_sun.y) * max(sky_toward_sun.y, 0)
        + sky_light_at(sky_viewer_radius(), sky_toward_moon.y) * sky_moon_illuminance * max(sky_toward_moon.y, 0)) * sky_scale;
    float3 body = lit_by * float3(0.004, 0.012, 0.014) * (1 - through);
    float3 bed = scene.Load(int3(pixel, 0));

    float3 sun = sky_light_at(sky_viewer_radius(), sky_toward_sun.y) * sky_scale;
    float3 moon = sky_light_at(sky_viewer_radius(), sky_toward_moon.y) * sky_moon_illuminance * sky_scale;
    float3 glints = glint(n_world, v_world, sky_toward_sun, sun) + glint(n_world, v_world, sky_toward_moon, moon);
    // The blend adds the change the water makes to the opaque scene, so what the game drew over that scene first, such as
    // ice it sorts before the water in some frames and after it in others, stays as it was.
    float3 water = min(fresnel * mirrored + glints + (1 - fresnel) * (body + bed * through), 60000);
    return float4(water - bed, 0);
}
