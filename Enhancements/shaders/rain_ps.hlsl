// Rain: streaks of falling drops in layers on cylinders around the camera, a meter and a half to 25 meters away, fixed in
// the world's directions so turning the camera turns through them. Each layer is cut
// into columns, each with its own speed and start, and those into cells, a share of which, by rain_amount and the gusts,
// holds one drop at a place, length and brightness of its own, slanted by the wind; nearer scenery hides a layer behind it.
// The drops are sparse, thin and faint, as a camera sees rain: the sky's light, sky_color, which small lenses show from the
// bright sky above, blurred over a twenty-fifth of a second. Drops land on upward ground near
// the camera as a bright crown and a ring. A thunderstorm's lightning flashes over all of it, brightest in the clouds
// toward its bolt, which is drawn in the open sky.
#include "common.hlsli"

Texture2D<float3> scene : register(t0);     // mirror of the HDR scene
Texture2D<float> depth : register(t2);      // mirror of the main depth, reversed Z

static const uint kLayers = 6;
static const float kNear = 1.5, kStep = 1.7;    // the layers' radii in meters: 1.5, 2.6, 4.3, 7.4, 12.5, 21.3
static const float kFall = 13;                  // a drop's mean speed in meters per second, a downpour's fastest
static const float kShutter = 1.0 / 25;         // the seconds a streak shows of a drop's fall, a film camera's blur
static const float kFill = 0.3;                 // the share of cells a downpour fills
static const float kDropWidth = 0.002;          // meters
static const float kSplashReach = 18;           // meters from the camera that splashes show
static const float kSplashCell = 0.35;          // meters of ground per landing spot

float2 hash2(float2 p)
{
    p = frac(p * float2(0.1031, 0.1030));
    p += dot(p, p.yx + 33.33);
    return frac((p.xx + p.yx) * p.yx);
}

float noise1(float x)
{
    float i = floor(x), f = frac(x);
    return lerp(hash2(float2(i, 3.7)).x, hash2(float2(i + 1, 3.7)).x, f * f * (3 - 2 * f));
}

float noise2(float2 p)
{
    float2 i = floor(p), f = frac(p);
    f = f * f * (3 - 2 * f);
    return lerp(lerp(hash2(i).x, hash2(i + float2(1, 0)).x, f.x), lerp(hash2(i + float2(0, 1)).x, hash2(i + 1).x, f.x), f.y);
}

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
    float3 drop = sky_color * 2.5 * rain_light;
    // Gusts: the rain thickens and thins in sheets drifting around the camera.
    float gust = lerp(0.45, 1.35, noise2(float2(azimuth * 1.6 - rain_time * 0.12, rain_time * 0.2)));

    // Splashes on upward ground near the camera: each spot of ground takes a drop every half second or so, at a place of its
    // own each time, as a crown at once and then a ring spreading out.
    if (!open_sky && scene_meters < kSplashReach && rain_amount > 0)
    {
        float up = saturate((abs(normal.y) - 0.6) / 0.3);
        float3 translation = float3(world_to_view._m03, world_to_view._m13, world_to_view._m23);
        float2 ground = mul(view - translation, (float3x3)world_to_view).xz;
        float2 spot = floor(ground / kSplashCell);
        float2 h = hash2(spot + 71.3);
        float period = 0.35 + 0.6 * h.x;
        float phase = rain_time / period + h.y;
        float cycle = floor(phase), age = frac(phase) * period;
        float2 pick = hash2(spot * 1.7 + cycle * 3.1);
        if (pick.x < rain_amount * gust)
        {
            float2 center = (spot + 0.3 + 0.4 * hash2(spot + cycle * 7.7)) * kSplashCell;
            float d = length(ground - center);
            float pixel_m = scene_meters * pixel_angle;
            float ring_width = max(0.008, pixel_m);
            float ring = saturate(1 - abs(d - (0.02 + age * 0.5)) / ring_width) * pow(saturate(1 - age / 0.22), 2) * (0.008 / ring_width);
            float crown = saturate(1 - d / max(0.015, pixel_m)) * saturate(1 - age / 0.07);
            float splash = (0.6 * ring + crown) * (0.6 + 0.4 * pick.y) * up * saturate((kSplashReach - scene_meters) / 6);
            color = lerp(color, sky_color * 3 * rain_light, saturate(splash) * 0.6);
        }
    }

    float flat = length(dir.xz);
    if (flat < 0.05 || rain_amount <= 0)
        return float4(color, 1);
    // The wind blows the drops aside as they fall: a slant of up to half a meter a meter.
    float2 wind = float2(cos(rain_wind), sin(rain_wind));
    float slant = 0.5 * dot(wind, float2(-sin(azimuth), cos(azimuth)));
    float cover = 0;
    float radius = kNear;
    [unroll] for (uint i = 0; i < kLayers; i++, radius *= kStep)
    {
        float t = radius / flat;
        if (t > scene_meters)
            break;
        // The layer's columns: drops a few centimeters apart near, wider far, where more of them fall in one pixel. Each
        // column falls at its own speed from its own start, so no two line up.
        float cell_w = 0.05 * sqrt(radius), cell_h = 1.2;
        float height = dir.y * t;
        float across = azimuth * radius + height * slant;
        float column = floor(across / cell_w);
        float2 rc = hash2(float2(column, i * 13.7 + 3.1));
        float speed = kFall * (0.75 + 0.5 * rc.x);
        float down = height + speed * rain_time + rc.y * 50;
        float row = floor(down / cell_h);
        float2 r = hash2(float2(column, row) + i * 41.7);
        float2 r2 = hash2(float2(row, column) * 1.37 + i * 9.1 + 5);
        if (r.x > rain_amount * gust * kFill)
            continue;
        float streak = speed * kShutter * (0.6 + 0.8 * r2.y);
        float start = r2.x * (cell_h - streak);
        float along = frac(down / cell_h) * cell_h - start;
        // The streak's width on screen: its own, or a pixel and a half's, with its light spread over that.
        float pixel_m = t * pixel_angle;
        float width = max(kDropWidth, pixel_m), share = sqrt(kDropWidth / width);
        float dx = abs(frac(across / cell_w) - (0.15 + 0.7 * r.y)) * cell_w;
        float taper = saturate(along / (streak * 0.3)) * saturate((streak - along) / (streak * 0.2));
        cover += saturate(1 - dx / (width * 0.75)) * taper * share * (0.5 + 0.5 * frac(r2.x * 7.3)) * saturate(1 - radius / 30);
    }
    // A drop bends the bright sky above toward the eye, a few times the light that reaches the air around it.
    return float4(lerp(color, drop, saturate(cover) * 0.35), 1);
}
