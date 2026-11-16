// The painted clouds, at full size: with a pattern texture, one layer at the clouds' middle height shows it, as Azurite
// Weathers II draws its sky, in place of the march. Over the sky's pixels (depth 0). Writes every pixel, because the
// result is copied over the game's HDR scene whole.
#include "gbuffer.hlsli"
#include "clouds.hlsli"
#include "scatter.hlsli"
#include "sky.hlsli"
#include "cloud_light.hlsli"

Texture2D<float3> scene : register(t0);     // mirror of the game's HDR scene

// A painted layer at distance t: its cover takes the light behind, more where the ray crosses it at a slant, and its
// painted gray says how deep in the cloud the light has gone and darkens it, so the painting's shading stays under our
// sun, which scatters through that depth, and our sky. Far off it fades into the sky behind.
float4 painted_layer(float t, float2 pattern, float3 dir, float c, float3 sun, float3 sky)
{
    float cover = 1 - pow(1 - min(pattern.x, 0.999), rsqrt(max(abs(dir.y), 0.05)));
    float lit = scattered(c, 1 + 8 * saturate(0.95 - pattern.y));
    float shade = pow(saturate(pattern.y / 0.8), 2);
    float taken = cover * exp(-t / cloud_fade);
    return float4(taken * shade * (sun * lit + sky * 0.6), 1 - taken);
}

float3 main(float4 position : SV_Position) : SV_Target0
{
    int2 pixel = int2(position.xy);
    float3 color = scene.Load(int3(pixel, 0));
    float3x3 to_world = transpose((float3x3)world_to_view);
    float3 origin = mul(to_world, -float3(world_to_view[0].w, world_to_view[1].w, world_to_view[2].w));
    float3 dir = normalize(mul(to_world, view_position(position.xy * render_size.zw, 1)));
    float mid = cloud_base + cloud_thickness * 0.5;
    float t = (mid - origin.y) / (origin.y < mid ? max(dir.y, 0.01) : min(dir.y, -0.01));
    float2 at = origin.xz + dir.xz * t + cloud_wind;
    float2 pattern = pattern_at(at);
    if (depth.Load(int3(pixel, 0)) > 0 || pattern.x <= 0.004)
        return color;
    float3 l = cloud_toward_sun;
    float c = dot(dir, l);
    float3 sun = sky_cloud_physical > 0.5 ? sunset_glow(layer_sunlight(origin, dir)) : kPi * sun_color * cloud_sun;
    float3 sky = sky_color * cloud_ambient;
    float4 cloud = painted_layer(t, pattern, dir, c, sun, sky);
    return color * cloud.a + cloud.rgb;
}
