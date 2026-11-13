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

// Cirrus weather: broad warped fields drawn out along the wind, where the march draws its tufts.
float cirrus_cells(float2 c)
{
    float2 uv = cloud_wind_space(c);
    float2 p = float2(uv.x / (cloud_size * 10 * cloud_stretch), uv.y / (cloud_size * 10));
    float wx = cloud_fbm(float3(p * 0.7, 0.5), 2) - 0.5;
    float wz = cloud_fbm(float3(p * 0.7 + float2(5.2, 7.1), 1.8), 2) - 0.5;
    float field = cloud_fbm(float3(p + float2(wx, wz), 2.5), 4);
    float low = 0.5 + (0.5 - cloud_coverage) * 0.45 - 0.12;
    return smoothstep(low, low + 0.4, field);
}

// One layer of cirrus tufts, at scale times cloud_size. Each cell, 10 by 2 sizes, may hold a head that trails a tail
// downwind: it widens, fades, curls across the wind and sways, and its strands run along it. A tail stays within the
// next cell downwind and one cell either side, so six cells cover every point. contrast is how clear the strands stand.
float cirrus_layer(float2 uv, float scale, int salt, float contrast)
{
    float s = cloud_size * scale;
    float2 cell_size = s * float2(10, 2);
    int2 at = int2(floor(uv / cell_size));
    float total = 0;
    [unroll] for (int di = -1; di <= 0; di++)
        [unroll] for (int dj = -1; dj <= 1; dj++)
        {
            int2 cell = at + int2(di, dj);
            uint2 a = cloud_hash(cell + salt * int2(1013, 7919));
            uint2 b = cloud_hash(int2(a.yx));
            float4 r = float4(a.x & 0xffffu, a.x >> 16, a.y & 0xffffu, a.y >> 16) / 65535.0;
            float4 q = float4(b.x & 0xffffu, b.x >> 16, b.y & 0xffffu, b.y >> 16) / 65535.0;
            float2 head = (cell + float2(r.x, 0.25 + 0.5 * r.y)) * cell_size;
            float d = uv.x - head.x;
            float t = d / (cell_size.x * lerp(0.4, 1, r.z));
            if (q.z >= 0.95 || t <= 0 || t >= 1)
                continue;
            float across = head.y + lerp(-0.3, 0.3, r.w) * cell_size.y * t * t + 0.15 * cell_size.y * sin(t * 5 + q.w * 6.28) * t;
            float e = (uv.y - across) / (cell_size.y * lerp(0.06, 0.18, q.x) * (0.35 + 2.5 * t));
            float spread = exp(-e * e);
            if (spread < 0.01)
                continue;
            float along = smoothstep(0, 0.06, t) * pow(1 - t, 1.3) * (1.6 - 0.6 * t);
            float strands = cloud_fbm(float3(d / (s * 4) + q.w * 50, (uv.y - across) / (s * 0.035) + q.z * 50, 3.3), 2);
            strands = lerp(1, pow(saturate((strands - 0.35) / 0.35), 1.5), contrast);
            total += lerp(0.3, 1, q.y) * along * spread * strands;
        }
    return total;
}

// Cirrus where its weather is: two layers of tufts over a faint veil. Far off, the strands average out.
float cirrus_body(float2 c, float weather, float distance)
{
    float2 uv = cloud_wind_space(c);
    float contrast = 0.7 * cloud_detail * saturate(40000 / distance);
    float tufts = cirrus_layer(uv, 1, 0, contrast) + 0.7 * cirrus_layer(uv, 0.45, 1, contrast);
    return weather * (1 - exp(-1.5 * tufts) + 0.3 * weather);
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
