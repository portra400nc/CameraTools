// The volume clouds' march, at half size: each ray from the game's camera crosses the layer between cloud_base and
// cloud_base + cloud_thickness in cloud_steps samples. The weather map says where clouds stand and how tall they grow, the
// shape noise gives them bodies and the detail noise eats their edges. Each sample inside a cloud looks toward the light
// through five samples spaced wider and wider, and is lit by the light through them with multiple scattering and by the sky.
// The result depends on the ray's direction alone, so clouds_resolve_ps.hlsl averages it over frames and
// clouds_apply_ps.hlsl filters it to full size.
#include "sun.hlsli"
#include "clouds.hlsli"
#include "scatter.hlsli"

Texture2D<float> cloud_map : register(t5);      // the weather map
Texture3D<float4> shape_noise : register(t7);   // cloud_shape_cs.hlsl's
Texture3D<float4> detail_noise : register(t8);  // cloud_detail_cs.hlsl's

static const float kShapeTile = 6;     // cloud sizes the shape noise spans before it repeats
static const float kDetailTile = 0.8;

float remap(float x, float a, float b, float c, float d) { return c + (x - a) / (b - a) * (d - c); }

static float2 skew_direction;
static float detail_weight = 1;

// Without the detail: the cloud body at p, 0 to 1, and the height in the layer and the weather there.
float cloud_body(float3 p, out float h, out float weather)
{
    h = (p.y - cloud_base) / cloud_thickness;
    weather = 0;
    if (h < 0 || h > 1)
        return 0;
    // Tops lean downwind.
    float2 c = p.xz + cloud_wind + skew_direction * h * cloud_thickness * 0.5;
    weather = cloud_map.SampleLevel(linear_clamp, (c - cloud_map_center) / cloud_map_extent + 0.5, 0);
    if (weather <= 0.01)
        return 0;
    float4 n = shape_noise.SampleLevel(linear_wrap, float3(c.x, p.y, c.y) / (cloud_size * kShapeTile), 0);
    float erosion = n.g * 0.625 + n.b * 0.25 + n.a * 0.125;
    float body = saturate(remap(n.r, erosion - 1, 1, 0, 1));
    // Flat bases, and tops that reach higher where the weather is heavier.
    float top = lerp(0.3, 1, weather);
    body *= smoothstep(0, 0.07, h) * smoothstep(top, top * 0.45, h);
    // Even under full weather the noise must carve the body, or the clouds turn into extruded slabs of the map.
    float coverage = weather * lerp(0.3, 0.8, cloud_coverage);
    return saturate(remap(body, 1 - coverage, 1, 0, 1));
}

float cloud_extinction(float3 p, out float h)
{
    float weather;
    float body = cloud_body(p, h, weather);
    if (body <= 0)
        return 0;
    float2 c = p.xz + cloud_wind + skew_direction * h * cloud_thickness * 0.5;
    float3 d = detail_noise.SampleLevel(linear_wrap, float3(c.x, p.y, c.y) / (cloud_size * kDetailTile), 0).rgb;
    float fine = d.r * 0.625 + d.g * 0.25 + d.b * 0.125;
    // Wispy at the base, billowing toward the top.
    fine = lerp(fine, 1 - fine, saturate(h * 4));
    body = saturate(remap(body, fine * 0.75 * cloud_detail * detail_weight, 1, 0, 1));
    return body * cloud_density;
}

float cloud_extinction_coarse(float3 p)
{
    float h, weather;
    return cloud_body(p, h, weather) * cloud_density;
}

// Optical depth from p toward the light, through samples at 5% to 94% of the layer's thickness along the light; only the
// nearest sees the detail.
float depth_toward_light(float3 p, float3 l)
{
    float depth = 0, from = 0, at = cloud_thickness * 0.05;
    [unroll] for (int j = 0; j < 5; j++)
    {
        float h;
        float sigma = j < 1 ? cloud_extinction(p + l * at, h) : cloud_extinction_coarse(p + l * at);
        depth += sigma * (at - from);
        from = at;
        at *= 2.1;
    }
    return depth;
}

// Multiple scattering after Wrenninge: each octave lets more light through, scatters less and more evenly.
float scattered(float c, float od)
{
    float lit = 0, a = 1, b = 1, g = 1;
    [unroll] for (int k = 0; k < 4; k++)
    {
        lit += b * (0.6 * henyey_greenstein(c, 0.8 * g) + 0.4 * henyey_greenstein(c, -0.15 * g)) * exp(-od * a);
        a *= 0.25;
        b *= 0.45;
        g *= 0.6;
    }
    return lit;
}

// Cirrus is a sheet too thin to march: the ray's crossing of the layer's middle stands in for the whole layer, its
// tufts thick by the ray's path through it, lit through half the layer toward the light.
float4 cirrus_ray(float3 origin, float3 dir, float t0, float t1, float3 l, float c, float3 sun, float3 sky)
{
    float t = (cloud_base + cloud_thickness * 0.5 - origin.y) / dir.y;
    if (t <= 0)
        return float4(0, 0, 0, 1);
    float3 p = origin + dir * t;
    float2 at = p.xz + cloud_wind;
    float weather = cloud_map.SampleLevel(linear_clamp, (at - cloud_map_center) / cloud_map_extent + 0.5, 0);
    if (weather <= 0.01)
        return float4(0, 0, 0, 1);
    float od = cirrus_body(at, weather, t) * cloud_density * (t1 - t0) * 0.55;
    float lit = scattered(c, weather * cloud_density * cloud_thickness * 0.5 / max(l.y, 0.2));
    float taken = (1 - exp(-od)) * exp(-t / cloud_fade);
    return float4(taken * (sun * lit + sky * 0.6), 1 - taken);
}

// Stratus is a sheet too: its optical thickness where the ray crosses the layer's middle, lit by the light that diffuses
// through it (two-stream, asymmetry 0.85), so thicker parts are darker, and by sun light scattered once through its
// thin parts, and by the gray sky light, which also gets through thin parts better. The light through a thick sheet is mostly gray, and the sheet about as bright as the clear sky it hides. Far off it fades into a gray haze rather than the sky,
// and it reaches the horizon.
float4 sheet_ray(float3 origin, float3 dir, float3 l, float c, float3 sun, float3 sky)
{
    float mid = cloud_base + cloud_thickness * 0.5;
    float t = (mid - origin.y) / (origin.y < mid ? max(dir.y, 0.01) : min(dir.y, -0.01));
    float2 at = origin.xz + dir.xz * t + cloud_wind;
    float weather = cloud_map.SampleLevel(linear_clamp, (at - cloud_map_center) / cloud_map_extent + 0.5, 0);
    if (weather <= 0.01)
        return float4(0, 0, 0, 1);
    float tau = stratus_thickness(at) * weather * cloud_density * cloud_thickness;
    float through = exp(-tau / max(abs(dir.y), 0.03));
    float mu = max(l.y, 0.05);
    float diffuse = mu / (1 + 0.75 * 0.15 * tau * (0.5 + 0.5 / mu));
    float lit = origin.y < mid ? diffuse : mu - diffuse;
    float single = henyey_greenstein(c, 0.8) * exp(-tau) * (1 - exp(-tau)) * 2;
    float3 gray = lerp(sky, dot(sky, float3(0.2126, 0.7152, 0.0722)), 0.8) * 0.15;
    float3 light = lerp(sun, dot(sun, float3(0.2126, 0.7152, 0.0722)), 0.6) * 0.04 * (lit + single * 0.5)
        + gray * lerp(1.5, 0.5, saturate(tau / 60));
    light = lerp(gray * 2.5, light, exp(-t / cloud_fade));
    return float4((1 - through) * light, through);
}

float4 main(float4 position : SV_Position) : SV_Target0
{
    // This frame's pixel of the 2x2 block of the half-size history that this quarter-size pixel covers.
    uint block = (uint)cloud_block;
    float2 pixel = floor(position.xy) * 2 + float2(block & 1, block >> 1) + 0.5;
    float2 uv = pixel * 2 * render_size.zw;
    float3 origin = game_b1[5].xyz;
    float3 dir = normalize(game_world(game_view_at(uv, 1)) - origin);
    float top = cloud_base + cloud_thickness;
    skew_direction = float2(cos(radians(cloud_pad)), sin(radians(cloud_pad)));
    if (cloud_shape >= 1.5)
        return sheet_ray(origin, dir, cloud_toward_sun, dot(dir, cloud_toward_sun), kPi * sun_color * cloud_sun, sky_color * cloud_ambient);
    float t0, t1;
    if (origin.y < cloud_base)
    {
        if (dir.y < 0.01)
            return float4(0, 0, 0, 1);
        t0 = (cloud_base - origin.y) / dir.y;
        t1 = (top - origin.y) / dir.y;
    }
    else if (origin.y > top)
    {
        if (dir.y > -0.01)
            return float4(0, 0, 0, 1);
        t0 = (top - origin.y) / dir.y;
        t1 = (cloud_base - origin.y) / dir.y;
    }
    else
    {
        t0 = 0;
        t1 = dir.y > 0 ? (top - origin.y) / dir.y : dir.y < 0 ? (cloud_base - origin.y) / dir.y : cloud_map_extent * 0.5;
    }
    t1 = min(t1, t0 + cloud_map_extent * 0.35);
    // Large steps through clear air; at the first body met, a step back and small steps, which resolve the cloud's skin
    // and its detail; back to large steps after a run of empty small ones. At most twice cloud_steps samples. A large
    // step is a twentieth of the distance, at least a fifth of a cloud and at most an even share of the slab: one that
    // spanned whole clouds near the camera found or missed their edges by the pixel's offset, which drew stripes.
    float budget = (t1 - t0) / (cloud_steps * 0.5);
    float fine = clamp(budget * 0.12, 10, 60);
    detail_weight = saturate(3 * cloud_size * kDetailTile / 32 / fine);
    // A random offset for each pixel and frame: a patterned one left its pattern, diagonal stripes, wherever the march
    // found or missed an edge by it.
    int seed = int(cloud_phase * 65536);
    float jitter = (cloud_hash(int2(pixel) + int2(seed * 7919, seed * 104729)).x & 0xffffu) / 65535.0;

    float3 l = cloud_toward_sun;
    float c = dot(dir, l);
    float3 sun = kPi * sun_color * cloud_sun;
    float3 sky = sky_color * cloud_ambient;
    if (cloud_shape >= 0.5)
        return cirrus_ray(origin, dir, t0, t1, l, c, sun, sky);

    float3 inscatter = 0;
    float transmittance = 1, opacity = 0;
    float least = cloud_size * 0.2;
    float coarse = min(max(t0 * 0.05, least), max(budget, least));
    float t = t0 + jitter * coarse;
    bool inside = false;
    uint empty = 0;
    for (uint i = 0; i < cloud_steps * 2 && t < t1 && transmittance > 0.01; i++)
    {
        float3 p = origin + dir * t;
        coarse = min(max(t * 0.05, least), max(budget, least));
        if (!inside)
        {
            if (cloud_extinction_coarse(p) <= 0)
            {
                t += coarse;
                continue;
            }
            inside = true;
            empty = 0;
            t = max(t - coarse, t0) + jitter * fine;
            continue;
        }
        // Deeper in, less of what a sample adds reaches the eye: the steps grow to four times as the light through falls,
        // so a whole overcast costs little more than broken clouds.
        float step = fine * min(rcp(max(transmittance, 0.25)), 4);
        float h;
        float sigma = cloud_extinction(p, h);
        if (sigma <= 0)
        {
            if (++empty > 8)
                inside = false;
            t += step;
            continue;
        }
        empty = 0;
        float od = depth_toward_light(p, l);
        float lit = scattered(c, od);
        // Light reaching a cloud's sunlit skin has not yet scattered toward the eye: the powder term darkens it a little.
        lit *= lerp(1, 1 - exp(-od * 2), 0.4);
        float3 light = sun * lit + sky * (0.2 + 0.8 * h);
        float through = exp(-sigma * step);
        float fade = exp(-t / cloud_fade);
        float taken = transmittance * (1 - through);
        inscatter += taken * light * fade;
        opacity += taken * fade;
        transmittance *= through;
        t += step;
    }
    return float4(inscatter, 1 - opacity);
}
