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
Texture3D<float> cloud_volume : register(t13);  // data/wdas_cloud: Disney's cloud, x across, y up, z along, with mips

static const float kShapeTile = 6;     // cloud sizes the shape noise spans before it repeats
static const float kCloudTypes = 5;    // ENHANCEMENTS_CLOUD_TYPE_COUNT

// What a type marches by, beside its height profile in cloud_types.
struct CloudKind
{
    float detail_tile;  // cloud sizes the erosion noise spans; its largest cells are half of it
    float2 cover;       // how far full weather raises the cover at no and at full Coverage
    float shortest;     // the share of the layer the weakest weather still raises a cloud through
    float erosion;      // how deep the erosion noise eats into a body at full erosion and Edge detail
    float lean;         // how far the tops lean downwind, as a share of the layer's thickness
    float swirl;        // how far the erosion's turbulence moves it, in erosion tiles
    // Disney's cloud in place of the noise (1), and how it stands: each a share of a grid cell long (packed: filling its
    // cell, edge to edge, without turning), a share of the layer tall, and how far its top spreads into an anvil.
    float volume;
    float2 size;
    float2 height;
    float packed;
    float anvil;
    float cell;         // cloud sizes a grid cell spans
    float masked;       // 1: the weather map, not Coverage alone, says which cells hold a cloud, in patches and bands
};

// By ENHANCEMENTS_CLOUD_TYPE_*. The towers (towering cumulus, cumulonimbus) stand on cloud_towers' domes: their cover
// passes 1 at a tower's middle so it stands solid and the shape noise forms billows over its sides, Coverage says how
// many towers stand, they reach many heights, and they stand nearly upright (at half the thickness a 6.5 km tower
// leaned 3 km, a slanted slab). Their erosion cells run from some 400 m down to 25 m.
// Every type stands Disney's cloud since the user found it the real one; volume 0 brings a type's noise clouds back (the
// git tag clouds-noise has them as they were tuned).
static const CloudKind kKinds[5] =
{
    { 0.8, float2(0.35, 0.85), 0.55, 0.35, 0.5, 0.15, 1, float2(0.4, 0.55), float2(0.3, 0.55), 0, 0, 6, 0 },     // cumulus
    // Altocumulus: the noise's soft overlapping puffs matched the user's photo, where Disney cloudlets read as stamps and
    // Disney's billows carved into the noise shredded the puffs at twice the cost (.work/clouds/kinds round12, round13).
    { 0.8, float2(0.35, 0.85), 0.55, 0.35, 0.5, 0.15, 0, float2(0.55, 0.75), float2(0.15, 0.45), 0, 0, 1, 1 },  // stratocumulus
    { 0.8, float2(0.35, 0.85), 0.55, 0.35, 0.5, 0.1, 1, float2(0.98, 0.98), float2(0.6, 1), 1, 0, 6, 0 },      // stratus
    { 0.6, float2(1.5, 1.5), 0.4, 0.6, 0.15, 0.25, 1, float2(0.4, 0.45), float2(0.55, 0.8), 0, 0.6, 6, 0 },  // cumulonimbus
    { 0.6, float2(1.5, 1.5), 0.3, 1.0, 0.1, 0.25, 1, float2(0.4, 0.55), float2(0.45, 1), 0, 0, 6, 0 },        // towering cumulus
};
static CloudKind kind_look;  // this type's, set by main

float remap(float x, float a, float b, float c, float d) { return c + (x - a) / (b - a) * (d - c); }

static float2 skew_direction;
static float detail_weight = 1;
static float coverage_boost = 0;

// Where the noises read p: its world place with the drift, the tops leaning downwind.
float3 cloud_space(float3 p, float h)
{
    float2 c = p.xz + cloud_wind + skew_direction * h * cloud_thickness * kind_look.lean;
    return float3(c.x, p.y, c.y);
}

static const float3 kVolumeVoxels = float3(250, 170, 307);
static float volume_lod = 0;    // the volume's mip the samples read: coarser for the search and the light far off

// Disney's cloud (a real cumulus, modeled after a photograph) stood in each cell of a grid of six cloud sizes, in as
// many cells as Coverage asks, as kind_look says: turned and mirrored, a share of the cell long and of the layer tall;
// or packed, filling its cell edge to edge, only flipped, for the layers. A cumulonimbus's top spreads into an anvil.
// Clouds built from noise read as sponge, popcorn or a cartoon's (.work/clouds/towers). Turned any way, the cloud's
// footprint reaches at most 0.645 of its length from its middle, so a middle 0.35 to 0.65 across its cell keeps it
// inside: one cell's lookup a sample, where nine cost the Deck 58 ms. c is the point in cloud space, y its height.
float cloud_in_cell(int2 at, float2 c, float y, float cell)
{
    uint2 a = cloud_hash(at), b = cloud_hash(at + int2(7919, 104729));
    float4 r = float4(a & 0xffffu, b & 0xffffu) / 65535.0;
    // Masked, as altocumulus's cloudlets fill patches and bands of the sky, the weather at the cell's middle (which
    // Coverage already thins or spreads) says how likely it holds one.
    float chance = cloud_coverage, grow = 1, stretch = 1, lift = 0;
    if (kind_look.masked > 0)
    {
        // The weather's edges are sharpened, so the sky opens in clear gaps between the patches, and broad noise makes some
        // patches of small cloudlets and some of big ones; each cloudlet is drawn out on its own and floats a little higher
        // or lower, so no two look alike and the layer is no flat sheet.
        float2 middle_of_cell = (at + 0.5) * cell;
        chance = smoothstep(0.15, 0.5, cloud_map.SampleLevel(linear_clamp, (middle_of_cell - cloud_map_center) / cloud_map_extent + 0.5, 0));
        grow = lerp(0.6, 1.2, cloud_fbm(float3(middle_of_cell / (cell * 5), 7.7), 2));
        stretch = lerp(0.7, 1.4, frac(r.w * 5.3));
        lift = frac(r.w * 11.9) * 0.4;
    }
    if (r.x >= chance)
        return 0;
    float length = cell * lerp(kind_look.size.x, kind_look.size.y, frac(r.w * 2.9)) * grow;
    float tall = cloud_thickness * lerp(kind_look.height.x, kind_look.height.y, frac(r.w * 7.31)) * lerp(0.6, 1, grow);
    bool packed = kind_look.packed > 0;
    float2 middle = packed || kind_look.anvil > 0 ? at + 0.5 : kind_look.masked > 0 ? at + 0.4 + 0.2 * r.yz : at + 0.35 + 0.3 * r.yz;
    float turn = packed ? (frac(r.w * 13.7) > 0.5 ? 3.1415927 : 0) : frac(r.w * 13.7) * 6.2831853;
    float2 d = c - middle * cell;
    float2 local = float2(cos(turn) * d.x + sin(turn) * d.y, cos(turn) * d.y - sin(turn) * d.x);
    local.x = frac(r.w * 3.1) > 0.5 ? -local.x : local.x;
    // Meters a voxel across and along: packed, the footprint stretched to the cell; else the cloud's own proportions.
    float2 across = packed ? length / kVolumeVoxels.xz : length / kVolumeVoxels.z;
    float3 uvw = float3(local.x / (across.x * kVolumeVoxels.x * stretch), (y - cloud_base - lift * cloud_thickness) / tall,
        local.y / (across.y * kVolumeVoxels.z));
    uvw.xz = uvw.xz / (1 + kind_look.anvil * smoothstep(0.6, 0.9, uvw.y)) + 0.5;
    return all(uvw >= 0) && all(uvw <= 1) ? cloud_volume.SampleLevel(linear_clamp, uvw, volume_lod) : 0;
}

float cloud_volume_at(float2 c, float y)
{
    float cell = cloud_size * kind_look.cell;
    if (kind_look.masked == 0)
        return cloud_in_cell(int2(floor(c / cell)), c, y, cell);
    // Masked cloudlets on a plain grid stood in rows like a lattice: their grid is bent by broad noise first. They are up
    // to 0.9 of a cell long (0.75 grown by up to 1.2), drawn out up to 1.4 times, with middles 0.4 to 0.6 across it, so neighbors touch and merge
    // into patches; a footprint then reaches 0.68 of a cell from its middle, under the 0.9 to the nearest cell the four
    // cells nearest the point leave out, so they hold every cloudlet that can reach it.
    c += (float2(cloud_fbm(float3(c / (cell * 7), 1.3), 2), cloud_fbm(float3(c / (cell * 7) + 5.1, 4.7), 2)) - 0.5) * cell * 3;
    int2 near = int2(floor(c / cell - 0.5));
    float best = 0;
    [unroll] for (int k = 0; k < 4; k++)
        best = max(best, cloud_in_cell(near + int2(k & 1, k >> 1), c, y, cell));
    return best;
}

// Without the erosion: the cloud body at p, 0 to 1, and the height in the layer and how much the type erodes there.
float cloud_body(float3 p, out float h, out float erosion)
{
    h = (p.y - cloud_base) / cloud_thickness;
    erosion = 0;
    if (h < 0 || h > 1)
        return 0;
    float3 c = cloud_space(p, h);
    // Disney's cloud carries its own detail, smooth and soft as a real cloud's: the noise's erosion only roughened it.
    if (kind_look.volume > 0)
        return cloud_volume_at(c.xz, p.y);
    float weather = cloud_map.SampleLevel(linear_clamp, (c.xz - cloud_map_center) / cloud_map_extent + 0.5, 0);
    if (weather <= 0.01)
        return 0;
    // Where the weather is weaker the clouds grow less tall: the type's profile is read higher up.
    uint kind = min((uint)cloud_type, 4u);
    float4 type = cloud_types.SampleLevel(linear_clamp, float2(h / lerp(kind_look.shortest, 1, weather), (kind + 0.5) / kCloudTypes), 0);
    erosion = type.g;
    // Even under full weather the cover stays below 1, so the noise carves the body rather than the clouds turning into
    // extruded slabs of the map; the type's spread adds to it.
    float cover = saturate(weather * lerp(kind_look.cover.x, kind_look.cover.y, cloud_coverage) + type.b + coverage_boost);
    float n = shape_noise.SampleLevel(linear_wrap, c / (cloud_size * kShapeTile), 0);
    return saturate(remap(n * type.r, 1 - cover, 1, 0, 1)) * cover;
}

float cloud_extinction(float3 p, out float h)
{
    float erosion;
    float body = cloud_body(p, h, erosion);
    if (body <= 0)
        return 0;
    float3 q = cloud_space(p, h) / (cloud_size * kind_look.detail_tile);
    // Turbulence, as Horizon Zero Dawn's clouds swirl their detail: the erosion read through a slow warp of itself, so
    // small billows tumble and fray at the rims instead of sitting as round balls.
    float3 swirl = float3(erosion_noise.SampleLevel(linear_wrap, q * 0.23 + float3(0.31, 0.57, 0.13), 0),
        erosion_noise.SampleLevel(linear_wrap, q * 0.23 + float3(0.71, 0.19, 0.83), 0), 0.5) - 0.5;
    float fine = erosion_noise.SampleLevel(linear_wrap, q + swirl.xzy * kind_look.swirl, 0);
    // Wispy at the base, billowing toward the top.
    fine = lerp(fine, 1 - fine, saturate(h * 4));
    body = saturate(remap(body, fine * kind_look.erosion * erosion * cloud_detail * detail_weight, 1, 0, 1));
    return body * cloud_density;
}

float cloud_extinction_coarse(float3 p)
{
    float h, erosion;
    volume_lod = 1;
    float body = cloud_body(p, h, erosion);
    volume_lod = 0;
    return body * cloud_density;
}

// Whether a large step's sample may be in a cloud: the body grown a little, so a sample beside a thin part still finds it.
bool cloud_near(float3 p)
{
    coverage_boost = 0.1;
    volume_lod = 3;
    float h, erosion;
    bool near = cloud_body(p, h, erosion) > 0;
    coverage_boost = 0;
    volume_lod = 0;
    return near;
}

// Optical depth from p toward the light, through six samples from 3% to 150% of a cloud's size along it: the nearest two,
// with the detail, shade a billow's far side and the hollows between billows; the rest, the cloud's body behind it.
static const float kLightTaps[6] = { 0.03, 0.085, 0.19, 0.38, 0.77, 1.54 };
// What the taps and the sky's samples are spaced by: a cloud's size, or its layer's thickness when that is smaller, so a
// flat layer is not stepped through in one tap and shaded in bands.
static float light_scale = 1000;
float depth_toward_light(float3 p, float3 l)
{
    float depth = 0, from = 0;
    [unroll] for (int j = 0; j < 6; j++)
    {
        float at = kLightTaps[j] * light_scale, h;
        float sigma = j < 2 ? cloud_extinction(p + l * at, h) : cloud_extinction_coarse(p + l * at);
        depth += sigma * (at - from);
        from = at;
    }
    return depth;
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
    return lit;
}

// The share of the sky's light reaching p, after Frostbite (Hillaire 2016, 5.6): from the sky above, stronger up the
// layer, and from the ground below, weaker, both isotropic; the light from above is dimmed by the cloud over p, through
// two samples a tenth and a third of a cloud's size up, so the hollows between billows and a tower's lower body darken
// (.work/clouds/towers sheet-m.png m4). Without it the sky lit every hollow as brightly as a billow's crown, which read
// as smooth, toy-like clouds.
float sky_reaching(float3 p, float h)
{
    float near = light_scale * 0.1, far = light_scale * 0.33;
    float above = cloud_extinction_coarse(p + float3(0, near, 0)) * near + cloud_extinction_coarse(p + float3(0, far, 0)) * (far - near);
    return (0.25 + 0.75 * pow(saturate(h), 0.7)) * lerp(0.3, 1, exp(-above * 0.3)) + 0.25 * (1 - h);
}

float4 main(float4 position : SV_Position) : SV_Target0
{
    // This frame's pixel of the 2x2 block of the half-size history that this quarter-size pixel covers, or with block 4
    // this half-size pixel itself, with block 5 this full-size one.
    uint block = (uint)cloud_block;
    float2 pixel = block > 3 ? floor(position.xy) + 0.5 : floor(position.xy) * 2 + float2(block & 1, block >> 1) + 0.5;
    float2 uv = pixel * (block > 4 ? 1 : 2) * render_size.zw;
    // The camera from our own constants, so the clouds draw also when the game renders no sun shadows, as after sunset.
    float3x3 to_world = transpose((float3x3)world_to_view);
    float3 origin = mul(to_world, -float3(world_to_view[0].w, world_to_view[1].w, world_to_view[2].w));
    float3 dir = normalize(sky_world_from_view(view_position(uv, 1)));
    float top = cloud_base + cloud_thickness;
    skew_direction = float2(cos(radians(cloud_pad)), sin(radians(cloud_pad)));
    kind_look = kKinds[min((uint)cloud_type, 4u)];
    light_scale = min(cloud_size, cloud_thickness * 0.5);
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
    detail_weight = saturate(3 * cloud_size * kind_look.detail_tile / 32 / fine);
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
        float3 light = (physical ? physical_sunlight(origin, p) : sun) * lit + sky * sky_reaching(p, h);
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
