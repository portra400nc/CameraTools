// The cloud layer's history, at half size. This frame's march, at a quarter of the size, refreshed one pixel of every 2x2
// block; that pixel blends it into last frame's average, read where the same direction lay last frame, and the other three
// keep the moved average. The clouds are far, so the camera's turn moves them on screen and its travel barely does. The
// average is held within the range of the march's 3x3 neighborhood, widened, so it cannot keep a cloud that drifted away.
#include "common.hlsli"
#include "cloud_filter.hlsli"
#include "sky_camera.hlsli"

Texture2D<float4> current : register(t5);   // quarter size
Texture2D<float4> history : register(t6);   // half size

float4 main(float4 position : SV_Position) : SV_Target0
{
    int2 q = int2(position.xy);
    uint block = (uint)cloud_block;
    int2 cell = q / 2;
    uint2 size;
    current.GetDimensions(size.x, size.y);
    float2 uv = position.xy * 2 * render_size.zw;
    float4 spread = current.SampleLevel(linear_clamp, uv, 0);
    if (cloud_blend >= 1)
        return spread;
    float4 low = 1e9, high = -1e9;
    [unroll] for (int y = -1; y <= 1; y++)
        [unroll] for (int x = -1; x <= 1; x++)
        {
            float4 v = current.Load(int3(clamp(cell + int2(x, y), 0, int2(size) - 1), 0));
            low = min(low, v);
            high = max(high, v);
        }
    float4 margin = (high - low) * 0.5;
    low -= margin;
    high += margin;

    float3 view = view_position(uv, 1);
    float3 world = sky_world_from_view(normalize(view));
    // The clouds drifted: last frame, what lies in this direction lay upwind, at the layer's middle.
    float mid = cloud_base + cloud_thickness * 0.5 - cloud_camera_height;
    float reach = abs(world.y) > 0.01 && mid * world.y > 0 ? min(mid / world.y, cloud_map_extent * 0.35) : cloud_map_extent * 0.35;
    world = normalize(world * reach + float3(cloud_wind_step.x, 0, cloud_wind_step.y));
    float3 before = sky_last_view_from_world(world);
    float4 clip = mul(cloud_prev_view_to_clip, float4(before * 1000, 1));
    if (clip.w <= 0)
        return spread;
    float2 ndc = clip.xy / clip.w;
    float2 back = stored_uv(float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5));
    if (any(back < 0) || any(back > 1))
        return spread;
    uint2 history_size;
    history.GetDimensions(history_size.x, history_size.y);
    float4 kept = clamp(sample_catmull_rom(history, back, float2(history_size)), low, high);
    bool fresh = all(uint2(q & 1) == uint2(block & 1, block >> 1));
    return fresh ? lerp(kept, current.Load(int3(cell, 0)), cloud_blend) : kept;
}
