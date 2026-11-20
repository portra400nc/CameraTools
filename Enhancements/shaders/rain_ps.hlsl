// Rain's fullscreen pass, before rain_drops_vs.hlsl draws the drops over it: drops landing on upward ground near the
// camera, as a bright crown and a ring, a share of them by rain_amount and the gusts, where the shelter map sees no cover over
// the ground. A thunderstorm's lightning flashes over all of it, brightest in the clouds toward its bolt, which is drawn in
// the open sky.
#include "common.hlsli"
#include "rain.hlsli"

Texture2D<float3> scene : register(t0);     // mirror of the HDR scene
Texture2D<float> depth : register(t2);      // mirror of the main depth, reversed Z

static const float kSplashReach = 18;           // meters from the camera that splashes show
static const float kSplashCell = 0.35;          // meters of ground per landing spot

// A bolt's sideways wander in radians at s, 0 at its top to 1 at the ground.
float bolt_wander(float s, float seed)
{
    float off = 0, amplitude = 0.07, frequency = 3;
    [unroll] for (int k = 0; k < 5; k++, amplitude *= 0.48, frequency *= 2.3)
        off += (noise1(s * frequency + seed * (k + 1)) - 0.5) * amplitude;
    return off;
}

float angle_between(float a, float b)
{
    float d = a - b;
    return abs(atan2(sin(d), cos(d)));
}

float4 main(float4 position : SV_Position) : SV_Target0
{
    int2 pixel = int2(position.xy);
    float3 color = scene.Load(int3(pixel, 0));
    float2 uv = position.xy * render_size.zw;
    float3x3 to_world = transpose((float3x3)world_to_view);
    float3 dir = normalize(mul(to_world, view_position(uv, 1)));
    float scene_depth = depth.Load(int3(pixel, 0));
    bool open_sky = scene_depth <= 0;
    float3 view = view_position(uv, max(scene_depth, 1e-6));
    float scene_meters = open_sky ? 1e9 : length(view);
    // The ground's tilt from the depth's slope, before any branch.
    float3 normal = mul(to_world, normalize(cross(ddy(view), ddx(view))));
    float pixel_angle = 1 / max(projection_scale, 1);
    float azimuth = atan2(dir.z, dir.x);
    float3 ahead = mul(to_world, view_position(float2(0.5, 0.5), 1));
    float bolt_azimuth = atan2(ahead.z, ahead.x) + rain_bolt_azimuth;
    float sky_light = max(dot(sky_color, 1.0 / 3), 1e-4);

    // Lightning: the flash lights the scene, the clouds most toward the strike, in a cold white.
    float toward_bolt = 0.5 + 0.5 * cos(azimuth - bolt_azimuth);
    float flash = rain_flash * (open_sky ? 1.5 + 4 * toward_bolt * toward_bolt : 1.2);
    float3 flash_tint = float3(0.85, 0.92, 1.1);
    color *= 1 + flash * flash_tint;
    if (rain_bolt > 0 && open_sky)
    {
        float elevation = asin(clamp(dir.y, -1, 1));
        float top = 0.28 + 0.12 * frac(rain_bolt_seed * 0.37);
        if (elevation > -0.02 && elevation < top)
        {
            float s = (top - elevation) / top;
            float trunk = angle_between(azimuth, bolt_azimuth + bolt_wander(s, rain_bolt_seed));
            // One branch forks off part way down and strays to one side.
            float fork = 0.25 + 0.35 * frac(rain_bolt_seed * 0.71), side = frac(rain_bolt_seed * 0.13) > 0.5 ? 1 : -1;
            float along = s - fork;
            float branch = along > 0 && along < 0.4
                ? angle_between(azimuth, bolt_azimuth + bolt_wander(fork, rain_bolt_seed) + side * along * 0.18 + bolt_wander(s, rain_bolt_seed + 17) * 0.6) + along * 0.004 : 1;
            float near = min(trunk, branch) * cos(elevation);
            float core = saturate(1 - near / max(0.0012, pixel_angle * 1.5));
            float glow = exp(-near / 0.012);
            color += (core * 60 + glow * 4) * rain_bolt * sky_light * flash_tint * saturate(s * 10);
        }
    }

    float rain_light = 1 + rain_flash * 1.2;

    // Splashes on upward ground near the camera: each spot of ground takes a drop every half second or so, at a place of its
    // own each time, as a crown at once and then a ring spreading out.
    if (!open_sky && scene_meters < kSplashReach && rain_amount > 0)
    {
        float up = saturate((abs(normal.y) - 0.6) / 0.3);
        float3 translation = float3(world_to_view._m03, world_to_view._m13, world_to_view._m23);
        float3 world = mul(view - translation, (float3x3)world_to_view);
        float2 ground = world.xz;
        // Dry under a roof or a canopy the shelter map saw well over this ground.
        float exposed = 1 - shelter_cover(world, float2(0.4, 0.2));
        float2 spot = floor(ground / kSplashCell);
        float2 h = hash2(spot + 71.3);
        float period = 0.35 + 0.6 * h.x;
        float phase = rain_time / period + h.y;
        float cycle = floor(phase), age = frac(phase) * period;
        float2 pick = hash2(spot * 1.7 + cycle * 3.1);
        if (exposed > 0 && pick.x < rain_amount * rain_gust(azimuth))
        {
            float2 center = (spot + 0.3 + 0.4 * hash2(spot + cycle * 7.7)) * kSplashCell;
            float d = length(ground - center);
            float pixel_m = scene_meters * pixel_angle;
            float ring_width = max(0.008, pixel_m);
            float ring = saturate(1 - abs(d - (0.02 + age * 0.5)) / ring_width) * pow(saturate(1 - age / 0.22), 2) * (0.008 / ring_width);
            float crown = saturate(1 - d / max(0.015, pixel_m)) * saturate(1 - age / 0.07);
            float splash = (0.6 * ring + crown) * (0.6 + 0.4 * pick.y) * up * exposed * saturate((kSplashReach - scene_meters) / 6);
            color = lerp(color, sky_color * 3 * rain_light, saturate(splash) * 0.6);
        }
    }

    return float4(color, 1);
}
