// Rain's shelter map, before the rain draws: each surface the depth buffer shows raises the height its cell of the map
// holds to its own, so the cell keeps the highest surface seen over it. A frame reads one pixel of each 2x2 block,
// shelter_phase's, so four frames read them all.
#include "common.hlsli"
#include "rain.hlsli"
#include "rain_layers.hlsli"

Texture2D<float> depth : register(t2);      // mirror of the main depth, reversed Z
Texture2D<uint2> stencil : register(t3);    // same mirror; stencil in .g
RWTexture2D<uint> map : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    uint2 pixel = id.xy * 2 + uint2(shelter_phase & 1u, shelter_phase >> 1);
    if (any(pixel >= uint2(render_size.xy)))
        return;
    float d = depth.Load(int3(pixel, 0));
    // The sky has no surface, and characters move: they would leave a column without rain wherever they stood.
    if (d <= 0 || (stencil.Load(int3(pixel, 0)).g & 0x04u) != 0)
        return;
    float3 view = view_position((pixel + 0.5) * render_size.zw, d);
    float3 translation = float3(world_to_view._m03, world_to_view._m13, world_to_view._m23);
    float3 world = mul(view - translation, (float3x3)world_to_view);
    int2 cell = shelter_cell_of(world.xz);
    if (any(cell < 0) || any(cell >= kShelterSide))
        return;
    uint height = shelter_encode(world.y);
    // Once the map has seen a place, its cells already hold their heights, and reading first spares most of the atomics.
    if (map[cell] < height)
        InterlockedMax(map[cell], height);
}
