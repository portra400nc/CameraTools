// The clouds in every direction from the camera, for the physical water's mirror and the traced shot's sky: one ray a
// texel of a 256 x 256 octahedral map (sky_octahedron.hlsli), an eighth of the texels a frame (cloud_block 0 to 7, 8 for
// all of them). rgb the light the clouds send back along the ray, a their opacity, so an unbound map reads as a clear
// sky. cloud_sky_cs.hlsl folds it over the physical sky's map for the trace.
#define CLOUD_LIBRARY
#include "clouds_ps.hlsl"
#include "sky_octahedron.hlsli"

float4 main(float4 position : SV_Position) : SV_Target0
{
    uint2 texel = uint2(position.xy);
    uint slice = (texel.x & 1u) | ((texel.y & 1u) << 1) | (((texel.x >> 1) & 1u) << 2);
    if (cloud_block < 7.5 && slice != uint(cloud_block))
        discard;
    // A fixed offset for each texel: the map holds still while the clouds do, so the water's mirror does not flicker and a
    // traced shot's samples meet the same sky in every pass.
    uint2 noise = cloud_hash(int2(texel));
    float jitter = (noise.x & 0xffffu) / 65535.0;
    light_spread = 0.6 + 0.8 * (noise.y & 0xffffu) / 65535.0;
    return cloud_ray(cloud_eye(), octahedron_direction(position.xy / kCloudSkySide), jitter, sky_cloud_physical > 0.5);
}
