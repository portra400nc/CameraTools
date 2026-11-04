// The trace pass's third kernel: a sample of each traced pixel's reflection (reflect.hlsli), over the gather's tiles, added to
// reflect_sum: its light summed, and the samples. Each group adds its rays, guard stops and stack overflows to the gather's
// counters. Registers stay inside gpu.h's ranges: t0-t10, u0-u5, b0-b4.
#include "gather_bindings.hlsli"

cbuffer Reflect : register(b4)
{
    float4 reflect_params;
};

Texture2D<float> smoothness : register(t9);     // mirror of G-buffer rt4
Texture2D<float> material : register(t10);      // mirror of G-buffer rt3
RWStructuredBuffer<float4> reflect_sum : register(u5);

float load_smoothness(int2 pixel) { return smoothness.Load(int3(pixel, 0)); }
uint load_material(int2 pixel) { return uint(material.Load(int3(pixel, 0)) * 255.0f + 0.5f) & 0x3Fu; }

#include "reflect.hlsli"

groupshared uint group_counts[3];

[numthreads(8, 8, 1)]
void main(uint3 group : SV_GroupID, uint3 thread : SV_GroupThreadID, uint lane : SV_GroupIndex)
{
    if (lane < 3u)
        group_counts[lane] = 0u;
    GroupMemoryBarrierWithGroupSync();
    uint tile = tile_list.Load(((trace_tiles.x + group.y) % trace_tiles.y) * 4u);
    int2 pixel = int2((tile & 0xFFFFu) * 64u + (group.x % 8u) * 8u + thread.x, (tile >> 16) * 64u + (group.x / 8u) * 8u + thread.y);
    if (pixel.x < int(trace_render.x) && pixel.y < int(trace_render.y))
    {
        uint at = uint(pixel.y) * trace_render.x + uint(pixel.x);
        float4 sum = reflect_sum[at];
        if (sum.w < float(gather_counts.x))
        {
            GatherData r = reflect_pixel_of(pixel, uint(sum.w), lane);
            if (r.world != 0u)
            {
                reflect_sum[at] = sum + float4(r.light.x, r.light.y, r.light.z, 1.0f);
                InterlockedAdd(group_counts[0], r.rays);
                InterlockedAdd(group_counts[1], r.guard);
                InterlockedAdd(group_counts[2], r.overflow);
            }
        }
    }
    GroupMemoryBarrierWithGroupSync();
    if (lane < 3u && group_counts[lane] != 0u)
        InterlockedAdd(counters[kTraceCounterCount + lane], group_counts[lane]);
}
