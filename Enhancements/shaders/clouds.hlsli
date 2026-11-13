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

// The weather map's value at a point of cloud space (world x and z plus the drift): 0 clear sky to 1 where the clouds are
// thickest and tallest. Broad warped patches some six cells across; the 3D noise gives the clouds inside them their shape.
float cloud_cells(float2 c)
{
    float2 wind = float2(cos(radians(cloud_pad)), sin(radians(cloud_pad)));
    float2 p = float2(dot(c, wind) / (cloud_size * cloud_stretch), dot(c, float2(-wind.y, wind.x)) / cloud_size) / 6;
    float wx = cloud_fbm(float3(p * 0.5, 0.5), 2) - 0.5;
    float wz = cloud_fbm(float3(p * 0.5 + float2(5.2, 7.1), 1.8), 2) - 0.5;
    p += float2(wx, wz) * 1.5;
    float f = cloud_fbm(float3(p, 2.5), 5);
    float low = 0.5 + (0.5 - cloud_coverage) * 0.45 - 0.12;
    return smoothstep(low, low + 0.35, f);
}
