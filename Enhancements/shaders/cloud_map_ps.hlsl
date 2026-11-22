// The cloud layer's first draw: the weather around the camera into a kCloudMapSize square map, cloud_map_extent meters
// across, which the march reads in place of computing it at every sample.
#include "common.hlsli"
#include "clouds.hlsli"

// The main layer's weather in x, and the layered sky's middle layer's in y.
float2 main(float4 position : SV_Position) : SV_Target0
{
    float2 c = cloud_map_center + (position.xy / kCloudMapSize - 0.5) * cloud_map_extent;
    use_layer(0);
    float main_layer = cloud_weather(c);
    if (cloud_layers < 1.5)
        return float2(main_layer, 0);
    use_layer(1);
    return float2(main_layer, cloud_weather(c));
}
