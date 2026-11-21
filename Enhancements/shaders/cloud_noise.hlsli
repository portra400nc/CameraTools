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

float worley_fbm(float3 uvw, int n)
{
    return tiling_worley(uvw, n) * 0.625 + tiling_worley(uvw, n * 2) * 0.25 + tiling_worley(uvw, n * 4) * 0.125;
}
