// The trace pass's kernel: one thread per pixel in 8x8 groups, 64 groups to a 64x64 tile, each group's tile from
// tile_list at trace_tiles.x + SV_GroupID.y, wrapping at trace_tiles.y. Each traced pixel adds one sample of each of
// its rays to its history (trace.hlsli), and each group adds its rays, guard stops, stack overflows and consistency
// checks into counters. Registers stay inside gpu.h's ranges: t0-t5, u0-u1, b0.

cbuffer Trace : register(b0)
{
    float4 clip_rows[4];
    float4 eye_position;
    float4 sun_cone;
    float4 trace_params;
    uint4 trace_counts;
    uint4 trace_tiles;
    uint4 trace_render;
};

ByteAddressBuffer nodes : register(t0);         // core/bvh.h's BvhNode, 64 bytes each
ByteAddressBuffer triangles : register(t1);     // core/bvh.h's BvhTriangle, 48 bytes each
Texture2D<float4> normals : register(t2);       // mirror of G-buffer rt0, world normal * 0.5 + 0.5
Texture2D<float> depth : register(t3);          // mirror of the main depth, reversed Z
Texture2D<uint2> stencil : register(t4);        // same mirror; stencil in .g
ByteAddressBuffer tile_list : register(t5);     // core/trace.h's tile_order, x | y << 16
RWStructuredBuffer<float4> history : register(u0);  // per pixel, row by row: occlusion sum, sun sum, their counts
RWStructuredBuffer<uint> counters : register(u1);   // core/trace.h's TraceCounter order

groupshared uint stack_lds[64 * 24];
groupshared uint group_counts[5];

uint4 node_row(uint node, uint row) { return nodes.Load4(node * 64u + row * 16u); }
uint4 triangle_row(uint index, uint row) { return triangles.Load4(index * 48u + row * 16u); }
float load_depth(int2 pixel) { return depth.Load(int3(pixel, 0)); }
float3 load_normal(int2 pixel) { return normals.Load(int3(pixel, 0)).xyz * 2.0f - 1.0f; }
uint load_stencil(int2 pixel) { return stencil.Load(int3(pixel, 0)).g; }

#define LOOP [loop]
#define STACK_STORE(lane, slot, value) stack_lds[(lane) * 24u + (slot)] = (value)
#define STACK_LOAD(lane, slot) stack_lds[(lane) * 24u + (slot)]
#include "trace.hlsli"

[numthreads(8, 8, 1)]
void main(uint3 group : SV_GroupID, uint3 thread : SV_GroupThreadID, uint lane : SV_GroupIndex)
{
    if (lane < 5u)
        group_counts[lane] = 0u;
    GroupMemoryBarrierWithGroupSync();
    uint tile = tile_list.Load(((trace_tiles.x + group.y) % trace_tiles.y) * 4u);
    int2 pixel = int2((tile & 0xFFFFu) * 64u + (group.x % 8u) * 8u + thread.x, (tile >> 16) * 64u + (group.x / 8u) * 8u + thread.y);
    if (pixel.x < int(trace_render.x) && pixel.y < int(trace_render.y))
    {
        uint at = uint(pixel.y) * trace_render.x + uint(pixel.x);
        PixelTraceData r = trace_pixel_of(pixel, history[at], lane);
        if (r.world != 0u)
        {
            history[at] = r.history;
            InterlockedAdd(group_counts[0], r.rays);
            InterlockedAdd(group_counts[1], r.guard);
            InterlockedAdd(group_counts[2], r.overflow);
            InterlockedAdd(group_counts[3], r.tested);
            InterlockedAdd(group_counts[4], r.off);
        }
    }
    GroupMemoryBarrierWithGroupSync();
    if (lane < 5u && group_counts[lane] != 0u)
        InterlockedAdd(counters[lane], group_counts[lane]);
}
