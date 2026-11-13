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

// The lattice repeats every kCloudNoiseSize; read along a tilted plane through it, a 2D pattern never repeats.
float cirrus_noise(float2 p, int octaves)
{
    return cloud_fbm(float3(p, dot(p, float2(0.137, 0.291)) + 3.3), octaves);
}

// The direction cirrus is drawn out in, in wind space: along the wind, with a broad bend and a smaller curl across it.
float2 cirrus_flow(float2 uv)
{
    float s = cloud_size;
    float k = (cloud_fbm(float3(uv / (s * 8), 4.4), 2) - 0.5) * 0.7 + (cloud_fbm(float3(uv / (s * 1.5), 6.1), 2) - 0.5) * 0.8;
    return float2(1, k) * rsqrt(1 + k * k);
}

// Where cirrus starts: warped patches drawn out along the wind, more of them with more coverage.
float cirrus_source(float2 uv)
{
    float2 w = uv / (cloud_size * float2(4, 1.5));
    float warp = cloud_fbm(float3(w * 0.5, 9.9), 2) - 0.5;
    float patch = cloud_fbm(float3(w + warp * float2(1, 0.5), 8.2), 3);
    float low = 0.5 - (cloud_coverage - 0.5) * 0.4;
    return smoothstep(low, low + 0.25, patch);
}

// Fine noise averaged along the local flow over some 700 m: irregular silky fibers. keep fades them out far off, where
// they are finer than a pixel.
float cirrus_fibers(float2 uv, float keep)
{
    float fiber = cloud_size * 0.03;
    float2 step = cirrus_flow(uv) * fiber;
    float total = 0;
    [loop] for (int k = 0; k < 24; k++)
        total += cirrus_noise((uv + step * (k - 11.5)) / fiber, 2);
    float x = saturate(0.5 + (total / 24 - 0.5) * 3.5);
    return 0.5 + (x - 0.5) * keep;
}

// Cirrus where its weather is: the patches gathered along the flow, far upstream and a little downstream, so each trails
// a tail downwind, times the fibers. Edge detail sets how clear the fibers stand out.
float cirrus_body(float2 c, float weather, float distance)
{
    float2 uv = cloud_wind_space(c);
    float step = cloud_size * 0.25;
    float total = 0, norm = 0;
    float2 at = uv;
    [loop] for (int k = 0; k < 32; k++)
    {
        if (k > 0)
            at -= cirrus_flow(at) * step;
        float w = exp(-k / 16.0);
        total += w * cirrus_source(at);
        norm += w;
    }
    at = uv;
    [loop] for (int j = 0; j < 4; j++)
    {
        at += cirrus_flow(at) * step;
        float w = exp(-j / 1.5);
        total += w * cirrus_source(at);
        norm += w;
    }
    float contrast = 0.7 * cloud_detail;
    float fibers = cirrus_fibers(uv, saturate(40000 / distance));
    return weather * total / norm * 2 * (1 - contrast + contrast * 2 * fibers);
}

// Stratus weather: where the sheet is, with soft-edged breaks that open up as coverage falls.
float stratus_cells(float2 c)
{
    float2 uv = cloud_wind_space(c) / cloud_size;
    float breaks = cloud_fbm(float3(uv / 5, 3.3), 4);
    float mottle = cloud_fbm(float3(uv / 3, 1.1), 4);
    float low = 0.5 - (cloud_coverage - 0.5) * 0.9;
    return smoothstep(low - 0.08, low + 0.08, breaks + (mottle - 0.5) * 0.3);
}

// The stratus sheet's optical thickness over its mean: log-normal mottling some three sizes across, fine lumps, and rolls
// across the wind in patches. Edge detail sets how strong the lumps and rolls are.
float stratus_thickness(float2 c)
{
    float2 uv = cloud_wind_space(c) / cloud_size;
    float mottle = cloud_fbm(float3(uv / 3, 1.1), 4);
    float fine = cloud_fbm(float3(uv * 4, 7.7), 3);
    float warp = (cloud_fbm(float3(uv / 2, 2.2), 2) - 0.5) * 0.8;
    float roll = 0.5 + 0.5 * sin(6.2831853 * (uv.x + warp) / 0.8);
    float rolls = smoothstep(0.45, 0.7, cloud_fbm(float3(uv / 4, 5.5), 2));
    return exp(2.5 * (mottle - 0.5) * 2 + 2 * cloud_detail * (fine - 0.5) * 2) * (1 - cloud_detail * roll * rolls);
}

// The weather map's value at a point of cloud space (world x and z plus the drift): 0 clear sky to 1 where the clouds are
// thickest and tallest. Broad warped patches some six cells across; the 3D noise gives the clouds inside them their shape.
float cloud_cells(float2 c)
{
    if (cloud_shape >= 1.5)
        return stratus_cells(c);
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
