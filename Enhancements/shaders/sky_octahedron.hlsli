// The octahedral map of directions that core/light.h's sky map uses (sky_direction, and shaders/gather.hlsli's
// sky_texel_of): the octahedron's upper half fills the diamond |u| + |v| <= 1 of the map, and its lower half is folded
// out into the four corners. uv runs from 0 to 1 across the map.

// The side of the clouds' sky map, cloud_sky_ps.hlsl's, twice the traced sky map's.
static const float kCloudSkySide = 256;

float3 octahedron_direction(float2 uv)
{
    float x = uv.x * 2 - 1, z = uv.y * 2 - 1;
    float y = 1 - abs(x) - abs(z);
    if (y < 0)
    {
        float fx = (1 - abs(z)) * (x >= 0 ? 1 : -1), fz = (1 - abs(x)) * (z >= 0 ? 1 : -1);
        x = fx;
        z = fz;
    }
    return normalize(float3(x, y, z));
}

float2 octahedron_uv(float3 d)
{
    float scale = 1 / (abs(d.x) + abs(d.y) + abs(d.z));
    float u = d.x * scale, v = d.z * scale;
    if (d.y < 0)
    {
        float fu = (1 - abs(v)) * (u >= 0 ? 1 : -1), fv = (1 - abs(u)) * (v >= 0 ? 1 : -1);
        u = fu;
        v = fv;
    }
    return float2(u, v) * 0.5 + 0.5;
}
