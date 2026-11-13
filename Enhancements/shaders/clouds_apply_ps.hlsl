// The cloud layer's third draw, at full size: the half-size march over the sky's pixels (depth 0). Writes every pixel,
// because the result is copied over the game's HDR scene whole.
#include "gbuffer.hlsli"
#include "cloud_filter.hlsli"

Texture2D<float3> scene : register(t0);     // mirror of the game's HDR scene
Texture2D<float4> clouds : register(t5);    // rgb the light the clouds send toward the camera, a what comes through them

float3 main(float4 position : SV_Position) : SV_Target0
{
    int2 pixel = int2(position.xy);
    float3 color = scene.Load(int3(pixel, 0));
    if (depth.Load(int3(pixel, 0)) > 0)
        return color;
    uint2 size;
    clouds.GetDimensions(size.x, size.y);
    float4 cloud = sample_catmull_rom(clouds, position.xy * render_size.zw, float2(size));
    cloud = float4(max(cloud.rgb, 0), saturate(cloud.a));
    return color * cloud.a + cloud.rgb;
}
