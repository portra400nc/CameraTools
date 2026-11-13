// Tiling 3D noise for the volume clouds' two textures, generated once by cloud_shape_cs.hlsl and cloud_detail_cs.hlsl.
// Every function takes a point of the unit cube and a cell count that divides the cube, and wraps at its faces.

uint3 cloud_pcg3d(uint3 v)
{
    v = v * 1664525u + 1013904223u;
    v.x += v.y * v.z;
    v.y += v.z * v.x;
    v.z += v.x * v.y;
    v ^= v >> 16u;
    v.x += v.y * v.z;
    v.y += v.z * v.x;
    v.z += v.x * v.y;
    return v;
}

float3 cloud_random3(int3 cell, int n, uint salt)
{
    int3 w = (cell % n + n) % n;
    return float3(cloud_pcg3d(uint3(w) + salt)) / 4294967295.0;
}

// 1 at a feature point, falling to 0 a cell away.
float tiling_worley(float3 uvw, int n)
{
    float3 p = uvw * n;
    int3 cell = int3(floor(p));
    float3 f = p - cell;
    float best = 9;
    for (int z = -1; z <= 1; z++)
        for (int y = -1; y <= 1; y++)
            for (int x = -1; x <= 1; x++)
            {
                int3 o = int3(x, y, z);
                float3 d = o + cloud_random3(cell + o, n, 0u) - f;
                best = min(best, dot(d, d));
            }
    return 1 - saturate(sqrt(best));
}

float tiling_perlin(float3 uvw, int n)
{
    float3 p = uvw * n;
    int3 cell = int3(floor(p));
    float3 f = p - cell;
    float3 u = f * f * f * (f * (f * 6 - 15) + 10);
    float corner[8];
    for (int k = 0; k < 8; k++)
    {
        int3 o = int3(k & 1, (k >> 1) & 1, k >> 2);
        float3 g = normalize(cloud_random3(cell + o, n, 7u) * 2 - 1 + 1e-4);
        corner[k] = dot(g, f - o);
    }
    float x00 = lerp(corner[0], corner[1], u.x), x10 = lerp(corner[2], corner[3], u.x);
    float x01 = lerp(corner[4], corner[5], u.x), x11 = lerp(corner[6], corner[7], u.x);
    return lerp(lerp(x00, x10, u.y), lerp(x01, x11, u.y), u.z);
}

float tiling_perlin_fbm(float3 uvw, int n, int octaves)
{
    float total = 0, amplitude = 1, norm = 0;
    for (int i = 0; i < octaves; i++)
    {
        total += amplitude * tiling_perlin(uvw, n << i);
        norm += amplitude;
        amplitude *= 0.5;
    }
    return total / norm;
}

float worley_fbm(float3 uvw, int n)
{
    return tiling_worley(uvw, n) * 0.625 + tiling_worley(uvw, n * 2) * 0.25 + tiling_worley(uvw, n * 4) * 0.125;
}
