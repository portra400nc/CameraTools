// The cloud layer's shape, shared by its draws: cloud_map_ps.hlsl bakes the weather around the camera into a 2D map,
// clouds_ps.hlsl marches the slab through it, and clouds_apply_ps.hlsl lays the result over the sky; with a pattern
// texture, clouds_painted_ps.hlsl draws the layer in their place.
// Include it after common.hlsli.

// The layer a draw works on: the main one (0) or the layered sky's middle one (1), as use_layer loads it.
static uint layer_index;
static float layer_coverage, layer_base, layer_thickness, layer_size, layer_stretch, layer_density, layer_detail, layer_type;

void use_layer(uint i)
{
    layer_index = i;
    layer_coverage = i == 0 ? cloud_coverage : cloud_mid_coverage;
    layer_base = i == 0 ? cloud_base : cloud_mid_base;
    layer_thickness = i == 0 ? cloud_thickness : cloud_mid_thickness;
    layer_size = i == 0 ? cloud_size : cloud_mid_size;
    layer_stretch = i == 0 ? cloud_stretch : cloud_mid_stretch;
    layer_density = i == 0 ? cloud_density : cloud_mid_density;
    layer_detail = i == 0 ? cloud_detail : cloud_mid_detail;
    layer_type = i == 0 ? cloud_type : cloud_mid_type;
}

Texture3D<float> lattice : register(t6);    // noise_lattice() in core/plan.h, as the light shafts read it
Texture2D<float4> pattern_texture : register(t11);  // the pattern's Azurite Weathers II texture: painted gray, alpha the cover
SamplerState linear_wrap : register(s3);
SamplerState anisotropic_wrap : register(s4);

static const float kCloudNoiseSize = 64;    // kNoiseSize in core/plan.h
static const uint kCloudMapSize = 2048;     // kCloudMapSize in gpu.cpp

float cloud_value_noise(float3 p)
{
    float3 cell = floor(p);
    float3 f = p - cell;
    return lattice.SampleLevel(linear_wrap, (cell + f * f * (3 - 2 * f) + 0.5) / kCloudNoiseSize, 0);
}

float cloud_fbm(float3 p, int octaves)
{
    float total = 0, amplitude = 0.5, norm = 0;
    for (int i = 0; i < octaves; i++)
    {
        total += amplitude * cloud_value_noise(p);
        norm += amplitude;
        p = p * 2.03 + float3(17.1, 3.7, 9.3);
        amplitude *= 0.5;
    }
    return total / norm;
}

uint2 cloud_hash(int2 cell)
{
    uint2 v = uint2(cell) * 1664525u + 1013904223u;
    v.x += v.y * 1664525u;
    v.y += v.x * 1664525u;
    v ^= v >> 16u;
    v.x += v.y * 1664525u;
    v.y += v.x * 1664525u;
    v ^= v >> 16u;
    return v;
}

// A point of cloud space in wind space: x along the wind, y across it.
float2 cloud_wind_space(float2 c)
{
    float2 wind = float2(cos(radians(cloud_pad)), sin(radians(cloud_pad)));
    return float2(dot(c, wind), dot(c, float2(-wind.y, wind.x)));
}

// Cloud sizes one pattern texture spans, by ENHANCEMENTS_CLOUD_PATTERN_*: a cloud of the painting about Cloud size across.
static const float kPatternTile[7] = { 1, 25, 25, 25, 45, 45, 40 };

// The pattern texture at a point of cloud space, its u along the wind: the cover, 0 to 1, as Coverage grows or thins it,
// and the painted gray. Two lookups, the second smaller, turned a little and moved, blend by broad noise, so the texture's
// repeat and its seams do not show. Anisotropic, with the pixel's derivatives: call it before any branch.
float2 pattern_at(float2 c)
{
    float tile = layer_size * kPatternTile[min((uint)cloud_pattern, 6u)];
    float2 uv = cloud_wind_space(c) / tile;
    float2 turned = float2(0.990 * uv.x - 0.140 * uv.y, 0.140 * uv.x + 0.990 * uv.y) / 0.62 + float2(0.37, 0.71);
    float4 a = pattern_texture.Sample(anisotropic_wrap, uv);
    float4 b = pattern_texture.Sample(anisotropic_wrap, turned);
    float4 p = lerp(a, b, smoothstep(0.4, 0.6, cloud_fbm(float3(uv / 1.3, 4.2), 2)));
    // Over 0.5 the cover thickens without spreading a veil over the painting's clear sky; under it the thin parts go.
    float k = 0.5 - layer_coverage;
    float cover = k >= 0 ? saturate((p.a - k) / (1 - k)) : saturate(p.a / (1 + k));
    return float2(cover, dot(p.rgb, float3(0.2126, 0.7152, 0.0722)));
}

// The towers' weather (towering cumulus and cumulonimbus): towers scattered one to a cell of six cloud sizes, standing in as many cells as
// Coverage asks, each a cluster of four domes around its middle, as a tower stacks turrets: a central one 0.55 to 1 high
// and three broad shoulders beside it, 0.3 to 0.7 of its height, every rim warped by broad noise. The domes fall off as 1 - d^2 toward their
// rims, so the shape noise forms billows over the sides. One dome a tower stood as the same bell everywhere, and a flat
// one (1 - d^4) as a capsule; the broad patches of cloud_weather rise to a plateau, which left smooth blobs or an overcast.
float cloud_towers(float2 c)
{
    float cell = layer_size * 6;
    float2 warp = float2(cloud_fbm(float3(c / cell * 1.7, 3.1), 2), cloud_fbm(float3(c / cell * 1.7 + 4.3, 6.7), 2)) - 0.5;
    float2 p = c / cell + warp * 0.35;
    int2 home = int2(floor(p));
    float w = 0;
    for (int y = -1; y <= 1; y++)
        for (int x = -1; x <= 1; x++)
        {
            int2 at = home + int2(x, y);
            uint2 a = cloud_hash(at), b = cloud_hash(at + int2(7919, 104729));
            float4 r = float4(a & 0xffffu, b & 0xffffu) / 65535.0;
            if (r.x >= layer_coverage)
                continue;
            float2 center = at + 0.3 + 0.4 * r.yz;
            float radius = 0.2 + 0.2 * r.w;
            float height = lerp(0.55, 1, frac(r.w * 7.31));
            for (int k = 0; k < 4; k++)
            {
                float4 q = float4(cloud_hash(at * 4 + int2(k * 31 + 5, k * 17 + 11)) & 0xffffu, cloud_hash(at * 4 + int2(k * 13 + 3, k * 29 + 7)) & 0xffffu) / 65535.0;
                // The first is the central turret; the others are broad shoulders to its sides, up to its radius out.
                float2 dome = center + (k == 0 ? 0 : (q.xy - 0.5) * 2.2 * radius);
                float size = radius * (k == 0 ? 0.7 : lerp(0.55, 0.85, q.z));
                float tall = height * (k == 0 ? 1 : lerp(0.3, 0.7, q.w));
                float d = length(p - dome) / size;
                w = max(w, tall * saturate(1 - d * d));
            }
            // A cumulonimbus's anvil: a thin, wide footprint around the tower, which the type's spread fills only near
            // the top of the layer.
            if (layer_type == 3)
            {
                float d = length(p - center) / (radius * 2.5);
                w = max(w, 0.08 * saturate(1 - d * d));
            }
        }
    return w;
}

static const float kRdrFirst = 5;   // ENHANCEMENTS_CLOUD_TYPE_RDR_CUMULUS, the first of the RDR2 style's five

bool is_rdr() { return layer_type >= kRdrFirst; }

// One type of the RDR2 style (Bauer, SIGGRAPH 2019, "Creating the Atmospheric World of Red Dead Redemption 2"): its
// weather, and the height profile a cloud stands by over it, as his cloud map and height LUT shape them.
struct RdrLook
{
    float mass;         // the masses' noise cell, in cloud sizes
    float stretch;      // how much longer than wide the masses are along the wind
    float ramp;         // the weather's rise past the coverage threshold: a small ramp stands crisp heaps, a large one soft
    float speckle;      // how far up the scattered small clouds' weather reaches, 0 none
    float cover;        // added to Coverage: an overcast closes, a storm cell stands alone
    float3 rise;        // the threshold over the layer's height h: x + y h + z h^2
    float shelf;        // a storm's shelf: how far the threshold falls near the base, spreading it wide
    float anvil;        // and near the top, spreading it into an anvil
    float carve;        // how deep the detail eats into the body
    float2 width;       // the carve's softness at the base and at the top
    float light;        // how much thicker the light lookup sees the cloud than the march
    float cells;        // how far the small noise breaks the masses into cells: a layer's lumpy rolls; 0 for the heaps,
                        // whose height it would raise into spires (.work/deck-screens/headings.png)
};

// Cumulus, stratocumulus, stratus, cumulonimbus, altocumulus. A threshold rising with the square of the height stands a
// dome over each rounded peak of the weather; a linear rise tapers it; a rise mostly linear and steep flattens a roll.
static const RdrLook kRdrLooks[5] =
{
    { 7, 1.4, 0.22, 0.12, 0, float3(0.08, 0.25, 0.65), 0, 0, 2.4, float2(0.1, 0.4), 20, 0 },
    { 4, 2.2, 0.3, 0.15, 0.15, float3(0.06, 0.6, 0.3), 0, 0, 2.0, float2(0.1, 0.35), 15, 0.55 },
    { 9, 2.0, 0.15, 0, 0.45, float3(0.05, 0.3, 0.2), 0, 0, 1.6, float2(0.12, 0.45), 45, 0.4 },
    { 10, 1.0, 0.25, 0, 0.1, float3(0.12, 0.2, 0.55), 0.85, 0.5, 2.0, float2(0.1, 0.4), 25, 0 },
    { 2.5, 1.6, 0.35, 0.6, -0.1, float3(0.1, 0.5, 0.35), 0, 0, 2.2, float2(0.18, 0.45), 10, 0.3 },
};

RdrLook rdr_look() { return kRdrLooks[clamp((int)layer_type - (int)kRdrFirst, 0, 4)]; }

// The RDR2 style's weather: broad masses, warped so they wander, and scattered small clouds, whose noise changes in place
// as the wind carries them, so clouds form, grow and dissolve on their own instead of sliding by as fixed shapes. The
// masses turn over in about forty cloud sizes of travel, the small clouds, two across, in ten, drifting a little across
// the wind. The masses rise straight from the noise, falling off from each peak as a dome needs: through a smoothstep
// they stood flat-topped and the threshold cut columns from them (.work/deck-screens/headings2.png); the small noise makes
// only low clouds of its own, as riding on the masses its narrow peaks rose as spires (headings.png).
float rdr_weather(float2 c)
{
    RdrLook k = rdr_look();
    float2 wind = float2(cos(radians(cloud_pad)), sin(radians(cloud_pad)));
    float2 p = float2(dot(c, wind) / (layer_size * layer_stretch * k.stretch), dot(c, float2(-wind.y, wind.x)) / layer_size);
    float t = cloud_evolve / max(layer_size, 1);
    float2 q = p / k.mass;
    q += (float2(cloud_fbm(float3(q * 0.5, 0.5 + t / 90), 2), cloud_fbm(float3(q * 0.5 + float2(5.2, 7.1), 1.8 + t / 90), 2)) - 0.5) * 0.8;
    float big = cloud_fbm(float3(q, t / 40), 4);
    float small = cloud_fbm(float3(p / 2.2 + float2(0, t * 0.02), t / 10 + 11.3), 3);
    float coverage = saturate(layer_coverage + k.cover);
    float low = 0.68 - coverage * 0.3;
    float masses = saturate((big - low) / k.ramp) * (1 - k.cells * (1 - smoothstep(0.35, 0.65, small)));
    float scattered = saturate((small - (0.7 - coverage * 0.25)) / 0.12) * k.speckle;
    return max(masses, scattered);
}

// The weather map's value at a point of cloud space (world x and z plus the drift): 0 clear sky to 1 where the clouds
// stand thickest and tallest, as Coverage spreads or thins them. Broad warped patches some six cells across, drawn out
// along the wind; the shape noise gives the clouds inside them their bodies.
float cloud_weather(float2 c)
{
    if (is_rdr())
        return rdr_weather(c);
    if (layer_type >= 3)    // ENHANCEMENTS_CLOUD_TYPE_CUMULONIMBUS and _TOWERING
        return cloud_towers(c);
    float2 wind = float2(cos(radians(cloud_pad)), sin(radians(cloud_pad)));
    float2 p = float2(dot(c, wind) / (layer_size * layer_stretch), dot(c, float2(-wind.y, wind.x)) / layer_size) / 6;
    float wx = cloud_fbm(float3(p * 0.5, 0.5), 2) - 0.5;
    float wz = cloud_fbm(float3(p * 0.5 + float2(5.2, 7.1), 1.8), 2) - 0.5;
    p += float2(wx, wz) * 1.5;
    float f = cloud_fbm(float3(p, 2.5), 5);
    float low = 0.5 + (0.5 - layer_coverage) * 0.45 - 0.12;
    return smoothstep(low, low + 0.35, f);
}
