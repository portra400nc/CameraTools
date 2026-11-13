// The cloud layer's history, at half size: this frame's march blended into last frame's average, which is read where the
// same direction lay last frame. The clouds are far, so the camera's turn moves them on screen and its travel barely does.
// Last frame's value is held within the range this frame's 3x3 neighborhood spans, widened, so the history cannot keep a
// cloud that drifted away.
#include "common.hlsli"

Texture2D<float4> current : register(t5);
Texture2D<float4> history : register(t6);

float4 main(float4 position : SV_Position) : SV_Target0
{
    int2 q = int2(position.xy);
    float4 now = current.Load(int3(q, 0));
    if (cloud_blend >= 1)
        return now;
    float4 low = now, high = now;
    [unroll] for (int y = -1; y <= 1; y++)
        [unroll] for (int x = -1; x <= 1; x++)
        {
            float4 v = current.Load(int3(q + int2(x, y), 0));
            low = min(low, v);
            high = max(high, v);
        }
    float4 margin = (high - low) * 0.5;
    low -= margin;
    high += margin;

    float2 uv = position.xy * 2 * render_size.zw;
    float3 view = view_position(uv, 1);
    float3 world = mul(normalize(view), (float3x3)world_to_view);
    float3 before = mul((float3x3)cloud_prev_world_to_view, world);
    float4 clip = mul(cloud_prev_view_to_clip, float4(before * 1000, 1));
    if (clip.w <= 0)
        return now;
    float2 ndc = clip.xy / clip.w;
    float2 back = stored_uv(float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5));
    if (any(back < 0) || any(back > 1))
        return now;
    float4 kept = clamp(history.SampleLevel(linear_clamp, back, 0), low, high);
    return lerp(kept, now, cloud_blend);
}
