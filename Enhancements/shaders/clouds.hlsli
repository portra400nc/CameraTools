// The cloud layer's shape, shared by its three draws: cloud_map_ps.hlsl bakes the cells into a 2D map around the camera
// each frame, clouds_ps.hlsl marches the slab through it, and clouds_apply_ps.hlsl lays the result over the sky.
// Include it after common.hlsli.

Texture3D<float> lattice : register(t6);    // noise_lattice() in core/plan.h, as the light shafts read it
SamplerState linear_wrap : register(s3);

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

// 1 at a cell's feature point, falling toward 0 at its edges.
float cloud_worley(float2 p)
{
    int2 cell = int2(floor(p));
    float2 f = p - cell;
    float best = 9;
    [unroll] for (int y = -1; y <= 1; y++)
        [unroll] for (int x = -1; x <= 1; x++)
        {
            float2 point_at = float2(x, y) + float2(cloud_hash(cell + int2(x, y)) & 0xffffu) / 65535.0;
            float2 d = point_at - f;
            best = min(best, dot(d, d));
        }
    return saturate(1 - sqrt(best));
}

// A point of cloud space in wind space: x along the wind, y across it.
float2 cloud_wind_space(float2 c)
{
    float2 wind = float2(cos(radians(cloud_pad)), sin(radians(cloud_pad)));
    return float2(dot(c, wind), dot(c, float2(-wind.y, wind.x)));
}

// Cirrus strokes and their fibers run along gently bent lines: the across coordinate after a broad bend.
float cirrus_bent(float2 uv)
{
    return uv.y + (cloud_fbm(float3(uv / (cloud_size * 8), 4.4), 2) - 0.5) * cloud_size * 1.6;
}

// Cirrus weather: warped fields drawn out along the wind and filled with bent strokes; the march adds the fibers.
float cirrus_cells(float2 c)
{
    float2 uv = cloud_wind_space(c);
    float s = cloud_size;
    float2 p = float2(uv.x / (s * 8 * cloud_stretch), uv.y / (s * 8));
    float wx = cloud_fbm(float3(p * 0.7, 0.5), 2) - 0.5;
    float wz = cloud_fbm(float3(p * 0.7 + float2(5.2, 7.1), 1.8), 2) - 0.5;
    float field = cloud_fbm(float3(p + float2(wx, wz), 2.5), 4);
    float low = 0.5 + (0.5 - cloud_coverage) * 0.45 - 0.12;
    // The strokes also bend on a smaller scale, which the fibers inside them do not follow.
    float bent = cirrus_bent(uv) + (cloud_fbm(float3(uv / (s * 2), 6.1), 2) - 0.5) * s * 0.6;
    float strokes = cloud_fbm(float3(uv.x / (s * 3), bent / (s * 1.5), 8.2), 3);
    return smoothstep(low, low + 0.35, field) * smoothstep(0.35, 0.65, strokes);
}

// The weather map's value at a point of cloud space (world x and z plus the drift): 0 clear sky to 1 where the clouds are
// thickest and tallest. Broad warped patches some six cells across; the 3D noise gives the clouds inside them their shape.
float cloud_cells(float2 c)
{
    if (cloud_shape >= 0.5)
        return cirrus_cells(c);
    float2 wind = float2(cos(radians(cloud_pad)), sin(radians(cloud_pad)));
    float2 p = float2(dot(c, wind) / (cloud_size * cloud_stretch), dot(c, float2(-wind.y, wind.x)) / cloud_size) / 6;
    float wx = cloud_fbm(float3(p * 0.5, 0.5), 2) - 0.5;
    float wz = cloud_fbm(float3(p * 0.5 + float2(5.2, 7.1), 1.8), 2) - 0.5;
    p += float2(wx, wz) * 1.5;
    float f = cloud_fbm(float3(p, 2.5), 5);
    float low = 0.5 + (0.5 - cloud_coverage) * 0.45 - 0.12;
    return smoothstep(low, low + 0.35, f);
}
