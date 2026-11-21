// The volume clouds' march, at a quarter of the size: each ray from the game's camera crosses the layer between cloud_base
// and cloud_base + cloud_thickness in cloud_steps samples. As Frostbite and Horizon Zero Dawn build them, the weather map
// says where clouds stand, their type how dense they are up the layer, the shape noise gives them bodies and the erosion
// noise eats their edges. Each sample inside a cloud looks toward the light through six samples spaced wider and wider,
// and is lit by the light through them with multiple scattering and by the sky. The result depends on the ray's direction
// alone, so clouds_resolve_ps.hlsl averages it over frames and clouds_apply_ps.hlsl filters it to full size.
#include "sun.hlsli"
#include "clouds.hlsli"
#include "scatter.hlsli"
#include "sky.hlsli"
#include "cloud_light.hlsli"
#include "sky_camera.hlsli"

Texture2D<float> cloud_map : register(t5);      // the weather map
Texture3D<float> shape_noise : register(t7);    // cloud_shape_cs.hlsl's
Texture3D<float> erosion_noise : register(t8);  // cloud_detail_cs.hlsl's
Texture2D<float4> cloud_types : register(t12);  // core/clouds.h's table: a row a type, r density, g erosion, b spread

static const float kShapeTile = 6;     // cloud sizes the shape noise spans before it repeats
// By ENHANCEMENTS_CLOUD_TYPE_*: cloud sizes the erosion noise spans. Its largest cells are half the tile; a towering
// cumulus's billows want some 230 m, where 0.8 cut it into 500 m chunks and 0.18 repeated as a lattice.
static const float kDetailTiles[5] = { 0.8, 0.8, 0.8, 0.8, 0.35 };
static float detail_tile = 0.8;  // this type's, set by main
static const float kCloudTypes = 5;    // ENHANCEMENTS_CLOUD_TYPE_COUNT
// By ENHANCEMENTS_CLOUD_TYPE_*: how far full weather raises the cover at no and at full Coverage, for the towering cumulus
// past 1 at a tower's middle so it stands solid and the shape noise forms billows over its sides (its Coverage says how
// many towers stand instead); and the share of the layer the weakest weather still raises a cloud through, so towers
// stand at many heights.
static const float2 kCover[5] = { float2(0.35, 0.85), float2(0.35, 0.85), float2(0.35, 0.85), float2(0.35, 0.85), float2(1.5, 1.5) };
static const float kShortest[5] = { 0.55, 0.55, 0.55, 0.55, 0.3 };
// By ENHANCEMENTS_CLOUD_TYPE_*: how deep the erosion noise eats into a body at full erosion and Edge detail. Seen from a
// few kilometers, a towering cumulus shows only its shape noise's largest billows unless the erosion cuts the smaller
// ones into them (.work/clouds/towers sheet-j.png).
static const float kErosionDepth[5] = { 0.35, 0.35, 0.35, 0.35, 1.0 };

float remap(float x, float a, float b, float c, float d) { return c + (x - a) / (b - a) * (d - c); }

static float2 skew_direction;
static float detail_weight = 1;
static float coverage_boost = 0;

// By ENHANCEMENTS_CLOUD_TYPE_*: how far the tops lean downwind, as a share of the layer's thickness. A towering cumulus
// 6.5 km tall leaned 3 km at half, a slanted slab.
static const float kLean[5] = { 0.5, 0.5, 0.5, 0.5, 0.1 };

// Where the noises read p: its world place with the drift, the tops leaning downwind.
float3 cloud_space(float3 p, float h)
{
    float2 c = p.xz + cloud_wind + skew_direction * h * cloud_thickness * kLean[min((uint)cloud_type, 4u)];
    return float3(c.x, p.y, c.y);
}

// Without the erosion: the cloud body at p, 0 to 1, and the height in the layer and how much the type erodes there.
float cloud_body(float3 p, out float h, out float erosion)
{
    h = (p.y - cloud_base) / cloud_thickness;
    erosion = 0;
    if (h < 0 || h > 1)
        return 0;
    float3 c = cloud_space(p, h);
    float weather = cloud_map.SampleLevel(linear_clamp, (c.xz - cloud_map_center) / cloud_map_extent + 0.5, 0);
    if (weather <= 0.01)
        return 0;
    // Where the weather is weaker the clouds grow less tall: the type's profile is read higher up.
    uint kind = min((uint)cloud_type, 4u);
    float4 type = cloud_types.SampleLevel(linear_clamp, float2(h / lerp(kShortest[kind], 1, weather), (kind + 0.5) / kCloudTypes), 0);
    erosion = type.g;
    // Even under full weather the cover stays below 1, so the noise carves the body rather than the clouds turning into
    // extruded slabs of the map; the type's spread adds to it.
    float cover = saturate(weather * lerp(kCover[kind].x, kCover[kind].y, cloud_coverage) + type.b + coverage_boost);
    float n = shape_noise.SampleLevel(linear_wrap, c / (cloud_size * kShapeTile), 0);
    return saturate(remap(n * type.r, 1 - cover, 1, 0, 1)) * cover;
}

float cloud_extinction(float3 p, out float h)
{
    float erosion;
    float body = cloud_body(p, h, erosion);
    if (body <= 0)
        return 0;
    float fine = erosion_noise.SampleLevel(linear_wrap, cloud_space(p, h) / (cloud_size * detail_tile), 0);
    // Wispy at the base, billowing toward the top.
    fine = lerp(fine, 1 - fine, saturate(h * 4));
    body = saturate(remap(body, fine * kErosionDepth[min((uint)cloud_type, 4u)] * erosion * cloud_detail * detail_weight, 1, 0, 1));
    return body * cloud_density;
}

float cloud_extinction_coarse(float3 p)
{
    float h, erosion;
    return cloud_body(p, h, erosion) * cloud_density;
}

// Whether a large step's sample may be in a cloud: the body grown a little, so a sample beside a thin part still finds it.
bool cloud_near(float3 p)
{
    coverage_boost = 0.1;
    float h, erosion;
    bool near = cloud_body(p, h, erosion) > 0;
    coverage_boost = 0;
    return near;
}

// Optical depth from p toward the light, through six samples from 3% to 150% of a cloud's size along it: the nearest two,
// with the detail, shade a billow's far side and the hollows between billows; the rest, the cloud's body behind it.
static const float kLightTaps[6] = { 0.03, 0.085, 0.19, 0.38, 0.77, 1.54 };
float depth_toward_light(float3 p, float3 l)
{
    float depth = 0, from = 0;
    [unroll] for (int j = 0; j < 6; j++)
    {
        float at = kLightTaps[j] * cloud_size, h;
        float sigma = j < 2 ? cloud_extinction(p + l * at, h) : cloud_extinction_coarse(p + l * at);
        depth += sigma * (at - from);
        from = at;
    }
    return depth;
}

// Sunlight scattered toward the eye after Frostbite (Hillaire 2016, 5.6): three octaves of multiple scattering, each
// through half the optical depth, half as strong and half as forward, of a forward and a weaker backward Henyey-Greenstein
// lobe. Halving the depth keeps a billow's shaded side dark, where the painted layer's quartering lit it nearly through.
float volume_scattered(float c, float od)
{
    float lit = 0, a = 1, b = 1, g = 1;
    [unroll] for (int k = 0; k < 3; k++)
    {
        lit += b * (0.55 * henyey_greenstein(c, 0.8 * g) + 0.45 * henyey_greenstein(c, -0.3 * g)) * exp(-od * a);
        a *= 0.5;
        b *= 0.5;
        g *= 0.5;
    }
    return lit;
}

float4 main(float4 position : SV_Position) : SV_Target0
{
    // This frame's pixel of the 2x2 block of the half-size history that this quarter-size pixel covers, or with block 4
    // this half-size pixel itself.
    uint block = (uint)cloud_block;
    float2 pixel = block > 3 ? floor(position.xy) + 0.5 : floor(position.xy) * 2 + float2(block & 1, block >> 1) + 0.5;
    float2 uv = pixel * 2 * render_size.zw;
    // The camera from our own constants, so the clouds draw also when the game renders no sun shadows, as after sunset.
    float3x3 to_world = transpose((float3x3)world_to_view);
    float3 origin = mul(to_world, -float3(world_to_view[0].w, world_to_view[1].w, world_to_view[2].w));
    float3 dir = normalize(sky_world_from_view(view_position(uv, 1)));
    float top = cloud_base + cloud_thickness;
    skew_direction = float2(cos(radians(cloud_pad)), sin(radians(cloud_pad)));
    detail_tile = kDetailTiles[min((uint)cloud_type, 4u)];
    bool physical = sky_cloud_physical > 0.5;
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
    detail_weight = saturate(3 * cloud_size * detail_tile / 32 / fine);
    // A random offset for each pixel and frame: a patterned one left its pattern, diagonal stripes, wherever the march
    // found or missed an edge by it.
    int seed = int(cloud_phase * 65536);
    float jitter = (cloud_hash(int2(pixel) + int2(seed * 7919, seed * 104729)).x & 0xffffu) / 65535.0;

    float3 l = cloud_toward_sun;
    float c = dot(dir, l);
    float3 sun = physical ? sunset_glow(layer_sunlight(origin, dir)) : kPi * sun_color * cloud_sun;
    float3 sky = sky_color * cloud_ambient;

    float3 inscatter = 0;
    float transmittance = 1, opacity = 0;
    float least = cloud_size * 0.2;
    float coarse = min(max(t0 * 0.05, least), max(budget, least));
    float t = t0 + jitter * coarse;
    bool inside = false;
    uint empty = 0;
    float found = 0;
    for (uint i = 0; i < cloud_steps * 2 && t < t1 && transmittance > 0.01; i++)
    {
        float3 p = origin + dir * t;
        coarse = min(max(t * 0.05, least), max(budget, least));
        if (!inside)
        {
            if (!cloud_near(p))
            {
                t += coarse;
                continue;
            }
            inside = true;
            empty = 0;
            found = t;
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
            // Small steps go on at least to where the large step found the cloud: giving up short of it skipped the cloud
            // for some pixels' offsets, which left dark specks.
            if (++empty > 8 && t > found)
                inside = false;
            t += step;
            continue;
        }
        empty = 0;
        float od = depth_toward_light(p, l);
        float lit = volume_scattered(c, od);
        float3 light = (physical ? physical_sunlight(origin, p) : sun) * lit + sky * (0.2 + 0.8 * h);
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
