// The cloud layer's shape, shared by its draws: cloud_map_ps.hlsl bakes the weather around the camera into a 2D map,
// clouds_ps.hlsl marches the slab through it, and clouds_apply_ps.hlsl lays the result over the sky; with a pattern
// texture, clouds_painted_ps.hlsl draws the layer in their place.
// Include it after common.hlsli.

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
    float tile = cloud_size * kPatternTile[min((uint)cloud_pattern, 6u)];
    float2 uv = cloud_wind_space(c) / tile;
    float2 turned = float2(0.990 * uv.x - 0.140 * uv.y, 0.140 * uv.x + 0.990 * uv.y) / 0.62 + float2(0.37, 0.71);
    float4 a = pattern_texture.Sample(anisotropic_wrap, uv);
    float4 b = pattern_texture.Sample(anisotropic_wrap, turned);
    float4 p = lerp(a, b, smoothstep(0.4, 0.6, cloud_fbm(float3(uv / 1.3, 4.2), 2)));
    // Over 0.5 the cover thickens without spreading a veil over the painting's clear sky; under it the thin parts go.
    float k = 0.5 - cloud_coverage;
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
    float cell = cloud_size * 6;
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
            if (r.x >= cloud_coverage)
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
            if (cloud_type == 3)
            {
                float d = length(p - center) / (radius * 2.5);
                w = max(w, 0.08 * saturate(1 - d * d));
            }
        }
    return w;
}

// The weather map's value at a point of cloud space (world x and z plus the drift): 0 clear sky to 1 where the clouds
// stand thickest and tallest, as Coverage spreads or thins them. Broad warped patches some six cells across, drawn out
// along the wind; the shape noise gives the clouds inside them their bodies.
float cloud_weather(float2 c)
{
    if (cloud_type >= 3)    // ENHANCEMENTS_CLOUD_TYPE_CUMULONIMBUS and _TOWERING
        return cloud_towers(c);
    float2 wind = float2(cos(radians(cloud_pad)), sin(radians(cloud_pad)));
    float2 p = float2(dot(c, wind) / (cloud_size * cloud_stretch), dot(c, float2(-wind.y, wind.x)) / cloud_size) / 6;
    float wx = cloud_fbm(float3(p * 0.5, 0.5), 2) - 0.5;
    float wz = cloud_fbm(float3(p * 0.5 + float2(5.2, 7.1), 1.8), 2) - 0.5;
    p += float2(wx, wz) * 1.5;
    float f = cloud_fbm(float3(p, 2.5), 5);
    float low = 0.5 + (0.5 - cloud_coverage) * 0.45 - 0.12;
    return smoothstep(low, low + 0.35, f);
}
