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

// The map's value at a point of cloud space (world x and z plus the drift): how far into a cloud the point lies, 0 outside
// every cloud. Continuous noise patches a few cells across, roughened by the cells, give sheets and bands rather than one
// ball a cell.
float cloud_cells(float2 c)
{
    float2 wind = float2(cos(radians(cloud_pad)), sin(radians(cloud_pad)));
    float2 p = float2(dot(c, wind) / (cloud_size * cloud_stretch), dot(c, float2(-wind.y, wind.x)) / cloud_size);
    float wx = cloud_fbm(float3(p * 0.25, 0.5), 2) - 0.5;
    float wz = cloud_fbm(float3(p * 0.25 + float2(5.2, 7.1), 1.8), 2) - 0.5;
    p += float2(wx, wz) * 2;
    float base = cloud_fbm(float3(p * 0.45, 2.5), 4);
    float cells = cloud_worley(p) * 0.6 + cloud_worley(p * 2.3 + float2(3.1, 1.7)) * 0.4;
    float large = cloud_fbm(float3(c / (cloud_size * 7), 4.5), 3);
    float shape = base * (0.55 + 0.45 * cells) * (0.5 + 0.5 * smoothstep(0.2, 0.8, large));
    return saturate((shape - (1 - cloud_coverage) * 0.62) / (cloud_coverage * 0.5));
}
