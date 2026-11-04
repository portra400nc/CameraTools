// The trace pass's second kernel: one bounce of diffuse light. The groups and tiles of trace_cs.hlsl: one thread per
// pixel in 8x8 groups, 64 groups to a 64x64 tile, each group's tile from tile_list at trace_tiles.x + SV_GroupID.y,
// wrapping at trace_tiles.y. Each traced pixel with fewer than gather_counts.x samples adds one (gather.hlsli): its
// occlusion sample and count into the trace's history, which trace_cs.hlsl then leaves alone, a hit's light and whether
// the sun lit it into gather_hit, and a miss's sky and its count into gather_sky. Each group adds its rays, guard stops,
// stack overflows, hits by where their light came from and noise sums into the counters after the trace kernel's.
// Registers stay inside gpu.h's ranges: t0-t8, u0-u3, b0-b3.

#include "gather_bindings.hlsli"

groupshared uint group_counts[kGatherCounterCount];

[numthreads(8, 8, 1)]
void main(uint3 group : SV_GroupID, uint3 thread : SV_GroupThreadID, uint lane : SV_GroupIndex)
{
    if (lane < kGatherCounterCount)
        group_counts[lane] = 0u;
    GroupMemoryBarrierWithGroupSync();
    uint tile = tile_list.Load(((trace_tiles.x + group.y) % trace_tiles.y) * 4u);
    int2 pixel = int2((tile & 0xFFFFu) * 64u + (group.x % 8u) * 8u + thread.x, (tile >> 16) * 64u + (group.x / 8u) * 8u + thread.y);
    if (pixel.x < int(trace_render.x) && pixel.y < int(trace_render.y))
    {
        uint at = uint(pixel.y) * trace_render.x + uint(pixel.x);
        float4 h = history[at];
        GatherData r = gather_next_of(pixel, h, lane);
        if (r.world != 0u)
        {
            GatherSums g = gather_add_of(h, gather_hit[at], gather_sky[at], r);
            history[at] = g.history;
            if (r.light.w < 1.5f)
            {
                gather_hit[at] = g.hit;
                InterlockedAdd(group_counts[kGatherSourceCounters + r.source], 1u);
            }
            else
                gather_sky[at] = g.sky;
            InterlockedAdd(group_counts[0], r.rays);
            InterlockedAdd(group_counts[1], r.guard);
            InterlockedAdd(group_counts[2], r.overflow);
            InterlockedAdd(group_counts[kGatherDeviationCounter], g.deviation);
            InterlockedAdd(group_counts[kGatherMeanCounter], g.mean);
        }
    }
    GroupMemoryBarrierWithGroupSync();
    if (lane < kGatherCounterCount && group_counts[lane] != 0u)
    {
        uint count = group_counts[lane];
        if (lane >= kGatherDeviationCounter)
            count = (count + (1u << (kNoiseShift - 1u))) >> kNoiseShift;
        InterlockedAdd(counters[kTraceCounterCount + lane], count);
    }
}
