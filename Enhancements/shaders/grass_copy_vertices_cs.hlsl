// The game's grass clump copies times over, for its own grass shaders to draw: each vertex of the bound mesh (44 bytes:
// position, normal, the blade's root x and z, its wind phase and height weight, a word the shaders do not read) once per
// copy. Each copy after the first turns by the golden angle, moves up to 0.35 m and shrinks by up to a quarter, its wind
// phase moved on so the copies do not sway as one.
Buffer<uint> source : register(t0);     // the game's vertex buffer, a word an element
RWBuffer<uint> copies : register(u0);   // ours: copies x vertices of the same layout

cbuffer GrassCopy : register(b0)
{
    uint4 count;    // x the source's vertices, y the copies
};

static const uint kWords = 11;

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    uint vertices = count.x, n = count.y, i = id.x;
    if (i >= vertices * n)
        return;
    uint k = i / vertices, from = (i % vertices) * kWords, to = i * kWords;
    float3 p = asfloat(uint3(source[from], source[from + 1], source[from + 2]));
    float3 normal = asfloat(uint3(source[from + 3], source[from + 4], source[from + 5]));
    float2 root = asfloat(uint2(source[from + 6], source[from + 7]));
    float2 wind = asfloat(uint2(source[from + 8], source[from + 9]));
    float angle = k * 2.39996323, s, c;
    sincos(angle, s, c);
    float shrink = 1 - 0.25 * k / n;
    float2 shift = float2(cos(angle * 1.7), sin(angle * 1.7)) * 0.35 * sqrt(float(k) / n);
    p = float3(dot(float2(c, -s), p.xz) * shrink + shift.x, p.y * shrink, dot(float2(s, c), p.xz) * shrink + shift.y);
    normal = float3(dot(float2(c, -s), normal.xz), normal.y, dot(float2(s, c), normal.xz));
    root = float2(dot(float2(c, -s), root), dot(float2(s, c), root)) * shrink + shift;
    wind.x = frac(wind.x + k * 0.618034);
    copies[to] = asuint(p.x);
    copies[to + 1] = asuint(p.y);
    copies[to + 2] = asuint(p.z);
    copies[to + 3] = asuint(normal.x);
    copies[to + 4] = asuint(normal.y);
    copies[to + 5] = asuint(normal.z);
    copies[to + 6] = asuint(root.x);
    copies[to + 7] = asuint(root.y);
    copies[to + 8] = asuint(wind.x);
    copies[to + 9] = asuint(wind.y);
    copies[to + 10] = source[from + 10];
}
