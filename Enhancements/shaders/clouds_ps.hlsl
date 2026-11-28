// The volume clouds' march, at a quarter of the size: each ray from the game's camera crosses the layer between layer_base
// and layer_base + layer_thickness in cloud_steps samples. In the RDR2 style (Bauer, SIGGRAPH 2019), the weather map
// says where clouds stand, their type's height profile how tall they stand over it, and displaced noise carves their
// billows. Each sample inside a cloud looks toward the light through six samples spaced wider and wider,
// and is lit by the light through them with multiple scattering and by the sky. The result depends on the ray's direction
// alone, so clouds_resolve_ps.hlsl averages it over frames and clouds_apply_ps.hlsl filters it to full size.
#include "sun.hlsli"
#include "clouds.hlsli"
#include "scatter.hlsli"
#include "sky.hlsli"
#include "cloud_light.hlsli"
#include "sky_camera.hlsli"

Texture2D<float2> cloud_map : register(t5);     // the weather map: x the main layer's, y the middle layer's
Texture3D<float> shape_noise : register(t7);    // cloud_shape_cs.hlsl's
static const float kShapeTile = 6;     // cloud sizes the shape noise spans before it repeats

// The weather map at a point of cloud space, for the layer the march is in.
float weather_at(float2 c)
{
    float2 w = cloud_map.SampleLevel(linear_clamp, (c - cloud_map_center) / cloud_map_extent + 0.5, 0);
    return layer_index == 0 ? w.x : w.y;
}

static float2 skew_direction;
static float march_fine = 60;   // this ray's small step, which the RDR2 style's detail must not be finer than
static const float kRdrBrightness = 0.55;
// The Density row's 0.1, the types' preset, over the 0.005 a meter the RDR2 style was tuned at: one of the march's 60 m
// steps at 0.1 was opaque and banded the tops (.work/deck-screens/mapped15.png).
static const float kRdrDensity = 0.05;
static float coverage_boost = 0;

// Where the noises read p: its world place with the drift, the tops leaning downwind.
float3 cloud_space(float3 p, float h)
{
    float2 c = p.xz + cloud_wind + skew_direction * h * layer_thickness * rdr_look().lean;
    return float3(c.x, p.y, c.y);
}

// The RDR2 style's body, after Bauer (SIGGRAPH 2019, slides 16 to 18): the weather map cut by a threshold that rises with
// height as the type's profile says, so a cloud stands on a broad flat base and rounds off higher up where only strong
// weather still clears the threshold. Strong weather stands tall, weak weather stays a low flat cloud. A storm's
// threshold falls near the base, spreading a wide shelf under the tower, and near the top, spreading its anvil.
float rdr_body(float2 c, float h)
{
    float weather = weather_at(c) + coverage_boost;
    if (weather <= 0.01)
        return 0;
    RdrLook k = rdr_look();
    float threshold = k.rise.x + k.rise.y * h + k.rise.z * h * h;
    // The shelf only in a thin band at the base, under a high threshold elsewhere, so the storm's skirt stands as a flat disc
    // around the tower, as the talk's storm does (slide 19), rather than a thick skirt (.work/rdr-clouds/compare-storm.png).
    threshold *= 1 - k.shelf * (1 - smoothstep(0.04, 0.1, h)) - k.anvil * smoothstep(0.72, 0.9, h);
    // A soft body, rising over a third of the weather past the threshold, so the carve cuts billows deep into it: solid
    // within a thin rim, the carve only frayed the rim and the clouds stood as smooth lozenges (.work/rdr-clouds/v4-mix1.png).
    return saturate((weather - threshold) / 0.35) * smoothstep(0, 0.04, h);
}

// The RDR2 style's detail: Perlin-Worley noise, read through a displacement made of two planar reads (the xy and xz
// planes, as Bauer builds a quasi 3D vector from two 2D lookups), which churns slowly as the clouds travel, carved out of
// the body over a width that grows toward the top, so bases stay crisp and tops soft.
float rdr_carve(float3 c, float h, float body)
{
    RdrLook k = rdr_look();
    float t = cloud_evolve / max(layer_size, 1);
    float3 q = c / (layer_size * kShapeTile);
    // The displacement is smooth, a few cloud sizes across: a fine noise's cells, read for it, laid the clouds in
    // thin horizontal strata (.work/deck-screens/mapped1.png).
    float2 xy = float2(shape_noise.SampleLevel(linear_wrap, float3(q.xy * 0.4, t * 0.01), 2),
        shape_noise.SampleLevel(linear_wrap, float3(q.xy * 0.4 + 0.43, t * 0.01 + 0.5), 2)) - 0.5;
    float2 xz = float2(shape_noise.SampleLevel(linear_wrap, float3(q.xz * 0.4 + 0.17, t * 0.013), 2),
        shape_noise.SampleLevel(linear_wrap, float3(q.xz * 0.4 + 0.61, t * 0.013 + 0.5), 2)) - 0.5;
    float3 displacement = float3(xy.x + xz.x, xy.y, xz.y) * 0.3;
    // At the mip whose texels span one of the ray's small steps, so the march resolves the billows it carves; with the
    // entry bisected, finer detail no longer bands. A second octave, half as deep, frays the rims.
    float lod = clamp(log2(march_fine * 128 / (layer_size * kShapeTile)), 0, 5);
    float n = shape_noise.SampleLevel(linear_wrap, q + displacement - float3(0, t * 0.004, 0), lod);
    n = n * 0.7 + 0.3 * shape_noise.SampleLevel(linear_wrap, q * 2.7 + displacement * 1.6 + float3(0.31, 0.17 - t * 0.008, 0.53), lod);
    n *= k.carve * layer_detail;
    // The carve's width, not Bauer's upper bound: (body - n) / (soft - n) turned negative wherever the noise passed the
    // softness and emptied the inside of the cloud along contours of height (.work/deck-screens/stripes-crop.png).
    return saturate((body - n) / lerp(k.width.x, k.width.y, h));
}

// Without the detail: the cloud body at p, 0 to 1, and the height in the layer.
float cloud_body(float3 p, out float h)
{
    h = (p.y - layer_base) / layer_thickness;
    if (h < 0 || h > 1)
        return 0;
    return rdr_body(cloud_space(p, h).xz, h);
}

float cloud_extinction(float3 p, out float h)
{
    float body = cloud_body(p, h);
    if (body <= 0)
        return 0;
    return rdr_carve(cloud_space(p, h), h, body) * layer_density * kRdrDensity * rdr_look().view;
}

float cloud_extinction_coarse(float3 p)
{
    float h;
    return cloud_body(p, h) * layer_density * kRdrDensity * rdr_look().view;
}

// Whether a large step's sample may be in a cloud: the body grown a little, so a sample beside a thin part still finds it.
bool cloud_near(float3 p)
{
    coverage_boost = 0.1;
    float h;
    bool near = cloud_body(p, h) > 0;
    coverage_boost = 0;
    return near;
}

// Optical depth from p toward the light, through six samples from 3% to 150% of a cloud's size along it: the nearest two,
// with the detail, shade a billow's far side and the hollows between billows; the rest, the cloud's body behind it.
// Beyond this many meters the light samples thin to three.
static const float kNearLight = 8000;
static const float kLightTaps[6] = { 0.03, 0.085, 0.19, 0.38, 0.77, 1.54 };
// What the taps and the sky's samples are spaced by: a cloud's size, or its layer's thickness when that is smaller, so a
// flat layer is not stepped through in one tap and shaded in bands.
static float light_scale = 1000;
// Scales the taps' and the sky samples' distances by 0.6 to 1.4, a fresh value each pixel and frame: at fixed distances a
// tap stepping across a cloud's edge jumped the shading, and the jumps lined up into contour bands over the shaded sides;
// varied, the history averages them into gradients.
static float light_spread = 1;
// Far off (far), three of the six samples, all coarse: a cloud kilometers away shows no billow's own shade.
float depth_toward_light(float3 p, float3 l, bool far)
{
    float depth = 0, from = 0;
    [loop] for (int j = far ? 1 : 0; j < 6; j += far ? 2 : 1)
    {
        float at = kLightTaps[j] * light_scale * light_spread, h;
        float sigma = j < 2 && !far ? cloud_extinction(p + l * at, h) : cloud_extinction_coarse(p + l * at);
        depth += sigma * (at - from);
        from = at;
    }
    // The RDR2 style is seen thin and shaded thick, as Bauer's clouds take their sun's transmittance from a lookup tuned
    // apart from the view: at one density, 0.1 a meter banded the tops at the march's steps (.work/deck-screens/mapped15.png)
    // and 0.005 left them flat white (mapped18.png).
    return depth * rdr_look().light / rdr_look().view;
}

// Sunlight scattered toward the eye after Frostbite (Hillaire 2016, 5.6): octaves of multiple scattering, each through
// a quarter of the optical depth, 0.7 as strong and half as forward, of a forward and a weaker backward Henyey-Greenstein
// lobe. A real cloud scatters its light a thousand times (Disney's reference renders it so) and glows white through:
// three octaves at half strength left the Disney cloud gray at noon.
float volume_scattered(float c, float od)
{
    float lit = 0, a = 1, b = 1, g = 1;
    [unroll] for (int k = 0; k < 5; k++)
    {
        lit += b * (0.55 * henyey_greenstein(c, 0.8 * g) + 0.45 * henyey_greenstein(c, -0.3 * g)) * exp(-od * a);
        a *= 0.25;
        b *= 0.7;
        g *= 0.5;
    }
    // Through a thick deck the light diffuses rather than dying away: two-stream transmission, 1 / (1 + 3/4 (1 - g) od) at
    // g = 0.85, of a white cloud's light, over half the optical depth, as our density 0.1 is some twice a real deck's. With
    // the octaves alone a rain deck's base went black at noon (.work/clouds/weather rainlook4); a real nimbostratus passes
    // some 15% and its base is the brightest thing under it.
    return max(lit, 0.24 / (1 + 0.056 * od));
}

// The share of the sky's light reaching p, after Frostbite (Hillaire 2016, 5.6): from the sky above, stronger up the
// layer, and from the ground below, weaker, both isotropic; the light from above is dimmed by the cloud over p, through
// two samples a tenth and a third of a cloud's size up, so the hollows between billows and a tower's lower body darken
// (.work/clouds/towers sheet-m.png m4). Without it the sky lit every hollow as brightly as a billow's crown, which read
// as smooth, toy-like clouds.
float sky_reaching(float3 p, float h)
{
    float near = light_scale * 0.1 * light_spread, far = light_scale * 0.33 * light_spread;
    float above = cloud_extinction_coarse(p + float3(0, near, 0)) * near + cloud_extinction_coarse(p + float3(0, far, 0)) * (far - near);
    // The RDR2 style's cloud over a point hides the sky as thick as it hides the sun: seen at the march's thin density,
    // the undersides took the whole sky's light and every cloud stood flat white (.work/rdr-clouds/v1-mix1.png).
    above *= rdr_look().light / rdr_look().view;
    return (0.25 + 0.75 * pow(saturate(h), 0.7)) * lerp(0.3, 1, exp(-above * 0.3)) + 0.25 * (1 - h);
}

// What the march carries from layer to layer along a ray.
struct MarchState
{
    float3 inscatter;
    float transmittance, opacity;
    float weighted, taken_all;     // the light-weighted distance's sum and weight (Hillaire 2016, 5.9)
};

// Where a ray from origin along dir crosses the current layer's slab, t0 to t1; false when it does not.
bool layer_span(float3 origin, float3 dir, out float t0, out float t1)
{
    float top = layer_base + layer_thickness;
    t0 = 0;
    t1 = 0;
    if (origin.y < layer_base)
    {
        if (dir.y < 0.01)
            return false;
        t0 = (layer_base - origin.y) / dir.y;
        t1 = (top - origin.y) / dir.y;
    }
    else if (origin.y > top)
    {
        if (dir.y > -0.01)
            return false;
        t0 = (top - origin.y) / dir.y;
        t1 = (layer_base - origin.y) / dir.y;
    }
    else
        t1 = dir.y > 0 ? (top - origin.y) / dir.y : dir.y < 0 ? (layer_base - origin.y) / dir.y : cloud_map_extent * 0.5;
    t1 = min(t1, t0 + cloud_map_extent * 0.35);
    return t1 > t0;
}

// Marches the current layer between t0 and t1 into m.
void march_layer(float3 origin, float3 dir, float t0, float t1, float jitter, bool physical, inout MarchState m)
{
    light_scale = min(layer_size, layer_thickness * 0.5);
    // Large steps through clear air; at the first body met, a step back and small steps, which resolve the cloud's skin
    // and its detail; back to large steps after a run of empty small ones. At most twice cloud_steps samples. A large
    // step is a twentieth of the distance, at least a fifth of a cloud and at most an even share of the slab: one that
    // spanned whole clouds near the camera found or missed their edges by the pixel's offset, which drew stripes.
    float budget = (t1 - t0) / (cloud_steps * 0.5);
    // Small steps of at most 60 m, or a twentieth of a cloud for big ones: a towering cumulus's long flat base, 3 km clouds,
    // took every sample of rays that grazed it at 60 m, and the turrets behind it never drew.
    float fine = clamp(budget * 0.12, 10, max(60, layer_size * 0.05));
    march_fine = fine;
    float3 l = cloud_toward_sun;
    float c = dot(dir, l);
    float3 sun = physical ? sunset_glow(layer_sunlight(origin, dir)) : kPi * sun_color * cloud_sun;
    float3 sky = sky_color * cloud_ambient;
    float least = layer_size * 0.2;
    float coarse = min(max(t0 * 0.05, least), max(budget, least));
    float t = t0 + jitter * coarse;
    bool inside = false;
    uint empty = 0, lit_samples = 0;
    float od = 0;
    float found = 0;
    float clear = t0;  // the last sample in clear air, which the RDR2 style refines its entry from
    bool was_clear = true;
    // With the physical sky's air over them, the clouds fade over the far two thirds of the march, which ends at 0.35 of
    // the map: the air takes the far ones into the haze, where the old fade thinned them from the start, and a fade over
    // only the last 30% left a bright band of layer ending above the horizon (.work/clouds/kinds round14).
    float reach = cloud_map_extent * 0.35;
    for (uint i = 0; i < cloud_steps * 2 && t < t1 && m.transmittance > 0.01; i++)
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
            clear = t;
            continue;
        }
        // Deeper in, less of what a sample adds reaches the eye: the steps grow to four times as the light through falls,
        // so a whole overcast costs little more than broken clouds.
        float step = fine * min(rcp(max(m.transmittance, 0.25)), 4);
        float h;
        float sigma = cloud_extinction(p, h);
        // The RDR2 style's smooth surfaces show where each ray enters them: entered anywhere within a small step, their
        // light banded the tops. As Bauer refines his steps (SIGGRAPH 2019, slide 50), three halvings between the last
        // clear sample and this one find the skin to an eighth of a step.
        if (sigma > 0 && was_clear && t > clear)
        {
            float a = clear, b = t;
            [unroll] for (int k = 0; k < 3; k++)
            {
                float mid = 0.5 * (a + b), hm;
                if (cloud_extinction(origin + dir * mid, hm) > 0)
                    b = mid;
                else
                    a = mid;
            }
            t = b;
            p = origin + dir * t;
            sigma = cloud_extinction(p, h);
        }
        was_clear = sigma <= 0;
        if (sigma <= 0)
        {
            clear = t;
            // Small steps go on at least to where the large step found the cloud: giving up short of it skipped the cloud
            // for some pixels' offsets, which left dark specks.
            if (++empty > 8 && t > found)
                inside = false;
            // Through empty air the small steps grow, up to a large step: a ray through the towering cumulus's bounding
            // cylinders, mostly air, crawled through each at small steps and ran out of samples before the turrets.
            t += min(step * (1 + 0.5 * empty), max(coarse, step));
            continue;
        }
        empty = 0;
        // The light toward the sun changes slowly along a ray inside a cloud: every other small sample takes the last one's,
        // which halves the light samples, the march's largest cost.
        if (lit_samples++ % 2 == 0)
            od = depth_toward_light(p, l, t > kNearLight);
        float lit = volume_scattered(c, od);
        float3 light = (physical ? physical_sunlight(origin, p) : sun) * lit + sky * sky_reaching(p, h);
        // The RDR2 style's clouds stand bright but short of white, so their gray shading shows: at full light they burned
        // to flat white over the sky (.work/rdr-clouds/v6-mix1.png).
        light *= kRdrBrightness;
        float through = exp(-sigma * step);
        float fade = physical ? 1 - smoothstep(0.3, 1, t / reach) : exp(-t / cloud_fade);
        float taken = m.transmittance * (1 - through);
        m.inscatter += taken * light * fade;
        m.opacity += taken * fade;
        m.weighted += taken * t;
        m.taken_all += taken;
        m.transmittance *= through;
        t += step;
    }
}

// The light the clouds send back along a ray from origin along dir, rgb, and their opacity, a: the layers it crosses,
// nearest first, as the layered sky's middle layer lies over the main one.
float4 cloud_ray(float3 origin, float3 dir, float jitter, bool physical)
{
    skew_direction = float2(cos(radians(cloud_pad)), sin(radians(cloud_pad)));
    MarchState m = { float3(0, 0, 0), 1, 0, 0, 0 };
    float s0, s1, u0 = 0, u1 = 0;
    use_layer(0);
    bool main_hit = layer_span(origin, dir, s0, s1);
    bool middle_hit = false;
    if (cloud_layers > 1.5)
    {
        use_layer(1);
        middle_hit = layer_span(origin, dir, u0, u1);
    }
    bool middle_first = middle_hit && (!main_hit || u0 < s0);
    [loop] for (uint k = 0; k < 2; k++)
    {
        uint layer = (k == 0) == middle_first ? 1 : 0;
        if (layer == 0 ? !main_hit : !middle_hit)
            continue;
        use_layer(layer);
        march_layer(origin, dir, layer == 0 ? s0 : u0, layer == 0 ? s1 : u1, jitter, physical, m);
    }
    if (physical && m.taken_all > 0)
    {
        float3 through;
        float3 air = aerial_light(m.weighted / m.taken_all * 0.001, dir, 1, through);
        m.inscatter = m.inscatter * through + air * m.opacity;
    }
    return float4(m.inscatter, m.opacity);
}

// The game's camera, from our own constants, so the clouds draw also when the game renders no sun shadows, as after sunset.
float3 cloud_eye()
{
    float3x3 to_world = transpose((float3x3)world_to_view);
    return mul(to_world, -float3(world_to_view[0].w, world_to_view[1].w, world_to_view[2].w));
}

#ifndef CLOUD_LIBRARY
float4 main(float4 position : SV_Position) : SV_Target0
{
    // This frame's pixel of the 2x2 block of the half-size history that this quarter-size pixel covers, or with block 4
    // this half-size pixel itself, with block 5 this full-size one.
    uint block = (uint)cloud_block;
    float2 pixel = block > 3 ? floor(position.xy) + 0.5 : floor(position.xy) * 2 + float2(block & 1, block >> 1) + 0.5;
    float2 uv = pixel * (block > 4 ? 1 : 2) * render_size.zw;
    float3 dir = normalize(sky_world_from_view(view_position(uv, 1)));
    // A random offset for each pixel and frame: a patterned one left its pattern, diagonal stripes, wherever the march
    // found or missed an edge by it.
    int seed = int(cloud_phase * 65536);
    uint2 noise = cloud_hash(int2(pixel) + int2(seed * 7919, seed * 104729));
    float jitter = (noise.x & 0xffffu) / 65535.0;
    light_spread = 0.6 + 0.8 * (noise.y & 0xffffu) / 65535.0;
    float4 c = cloud_ray(cloud_eye(), dir, jitter, sky_cloud_physical > 0.5);
    return float4(c.rgb, 1 - c.a);
}
#endif
