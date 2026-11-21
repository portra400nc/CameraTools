// The cloud layer's first draw: the weather around the camera into a kCloudMapSize square map, cloud_map_extent meters
// across, which the march reads in place of computing it at every sample.
#include "common.hlsli"
#include "clouds.hlsli"

float main(float4 position : SV_Position) : SV_Target0
{
    float2 c = cloud_map_center + (position.xy / kCloudMapSize - 0.5) * cloud_map_extent;
    return cloud_weather(c);
}
