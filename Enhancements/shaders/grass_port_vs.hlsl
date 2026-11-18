// The grass port's vertex shader: a Cathedral clump at each of the game's grass blades, drawn by the game's own indirect
// grass draw with its constants and instance buffers bound. It places the clump as the game's grass vertex shaders
// (bc45d83a69036a07 and its variants) place their blades: the instance's x and z, the ground's height between the four
// corner heights packed in the instance, its size and turn, all relative to the grass origin at cb1[10], and projects with
// the game's camera rows. The game's wind is left out.
cbuffer GrassMaterial : register(b0) { float4 material[98]; };   // the game's cb0
cbuffer GrassFrame : register(b1) { float4 frame[11]; };         // cb1: [5] the camera, [10] the grass origin
cbuffer GrassCamera : register(b2) { float4 camera[25]; };       // cb2: [21..24] the projection's rows
cbuffer GrassDraw : register(b3) { uint4 draw; };                // cb3: x the draw's first instance

struct Instance
{
    float x, z, base;
    uint corners;           // four ground heights, a byte each, 0.0215686 m a step over base, less 2.75 m
    uint shape;             // byte 0 size, byte 1 height, bits 16-31 the turn
    uint3 colors;
};
StructuredBuffer<uint2> visible : register(t4);     // the visible list: x the draw's first index into it
StructuredBuffer<Instance> instances : register(t5);

struct Vertex
{
    float3 position : POSITION;     // meters, y up, the clump's own
    float3 normal : NORMAL;
    float2 root : TEXCOORD0;        // the blade's root x and z in the clump
    float2 uv : TEXCOORD1;
    uint instance : SV_InstanceID;
};

struct Blade
{
    float4 position : SV_Position;
    float3 normal : NORMAL;         // world
    float2 uv : TEXCOORD0;
    float height : TEXCOORD1;       // meters over the ground
    float rise : TEXCOORD2;         // 0 at the clump's foot, 1 at its top
    float3 tip : TEXCOORD3;         // the game's colors for the blade, as its grass shaders blend them
    float3 root : TEXCOORD4;
};

Blade main(Vertex v)
{
    Instance i = instances[v.instance + draw.x + visible[asuint(material[50].z)].x];
    float size = (i.shape & 255u) / 255.0 * material[48].y + material[48].x;
    float tall = size * (((i.shape >> 8) & 255u) / 255.0 * material[48].w + material[48].z);
    float turn = (i.shape >> 16) / 65535.0 * material[47].w + material[47].z;
    float s, c;
    sincos(turn, s, c);
    float2 across = float2(c, s) * size, along = float2(-s, c) * size;
    float3 origin = frame[10].xyz;
    float2 at = float2(i.x, i.z) - origin.xz;
    float2 local = float2(dot(across, v.position.xz), dot(along, v.position.xz)) + at;
    float2 root = float2(dot(across, v.root), dot(along, v.root)) + at;
    // The ground under the blade's root, between the instance's corner heights over its 1 m cell.
    float2 cell = root + origin.xz - floor(float2(i.x, i.z));
    float4 h = float4(i.corners & 255u, (i.corners >> 8) & 255u, (i.corners >> 16) & 255u, i.corners >> 24) * 0.0215686 + i.base - 2.75;
    float ground = lerp(lerp(h.x, h.y, cell.x), lerp(h.z, h.w, cell.x), cell.y);
    float3 p = float3(local.x, ground - origin.y + v.position.y * tall, local.y) - frame[5].xyz;
    Blade o;
    o.position = p.x * camera[21] + p.y * camera[22] + p.z * camera[23] + camera[24];
    o.normal = normalize(float3(dot(float2(c, s), v.normal.xz), v.normal.y, dot(float2(-s, c), v.normal.xz)));
    o.uv = v.uv;
    o.height = v.position.y * tall;
    o.rise = saturate(v.position.y / 0.8);
    o.tip = float3(i.colors.x & 255u, (i.colors.x >> 8) & 255u, (i.colors.x >> 16) & 255u) / 255;
    o.root = float3(i.colors.y & 255u, (i.colors.y >> 8) & 255u, (i.colors.y >> 16) & 255u) / 255;
    return o;
}
