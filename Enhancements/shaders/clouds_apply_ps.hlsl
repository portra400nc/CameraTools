// The cloud layer's third draw, at full size: the half-size march over the sky's pixels (depth 0). Writes every pixel,
// because the result is copied over the game's HDR scene whole.
#include "gbuffer.hlsli"

Texture2D<float3> scene : register(t0);     // mirror of the game's HDR scene
Texture2D<float4> clouds : register(t5);    // rgb the light the clouds send toward the camera, a what comes through them

float3 main(float4 position : SV_Position) : SV_Target0
{
    int2 pixel = int2(position.xy);
    float3 color = scene.Load(int3(pixel, 0));
    if (depth.Load(int3(pixel, 0)) > 0)
        return color;
    float4 cloud = clouds.SampleLevel(linear_clamp, position.xy * render_size.zw, 0);
    return color * cloud.a + cloud.rgb;
}
