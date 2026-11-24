// The pixel a trace, gather or reflection thread works on. An 8x8 group's tile is tile_list's entry trace_tiles.x +
// SV_GroupID.y, wrapping at trace_tiles.y, as x | y << 16 in 64x64 tiles, and the group is SV_GroupID.x of its 64. With
// trace_tiles.z at 2, an anchored shot's move, tile_list is instead the pixels the move's reprojection found its anchor
// did not see: their count, then each as x | y << 16, 4096 to a SV_GroupID.y. A thread past the count gets a pixel
// outside the render, which every kernel skips.
#ifndef WORK_PIXEL_HLSLI
#define WORK_PIXEL_HLSLI

int2 work_pixel(uint3 group, uint3 thread, uint lane)
{
    if (trace_tiles.z == 2u)
    {
        uint index = group.y * 4096u + group.x * 64u + lane;
        if (index >= tile_list.Load(0))
            return int2(trace_render.xy);
        uint p = tile_list.Load((index + 1u) * 4u);
        return int2(p & 0xFFFFu, p >> 16);
    }
    uint tile = tile_list.Load(((trace_tiles.x + group.y) % trace_tiles.y) * 4u);
    return int2((tile & 0xFFFFu) * 64u + (group.x % 8u) * 8u + thread.x, (tile >> 16) * 64u + (group.x / 8u) * 8u + thread.y);
}

#endif
