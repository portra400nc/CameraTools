// Rain: streaks of falling drops in layers on cylinders around the camera, a meter and a half to 25 meters away, fixed in
// the world's directions so turning the camera turns through them. Each layer is cut into cells as wide as its drops are
// apart, a share of which, by rain_amount, holds a drop falling at its speed, drawn as a streak as long as it falls in a
// sixtieth of a second and slanted by the wind; nearer scenery hides a layer behind it. The drops take the sky's light,
// sky_color, as small lenses show the bright sky above.
#include "common.hlsli"

Texture2D<float3> scene : register(t0);     // mirror of the HDR scene
Texture2D<float> depth : register(t2);      // mirror of the main depth, reversed Z

static const uint kLayers = 6;
static const float kNear = 1.5, kStep = 1.7;    // the layers' radii in meters: 1.5, 2.6, 4.3, 7.4, 12.5, 21.3
static const float kFall = 9;                   // a drop's speed in meters per second
static const float kStreak = kFall / 60;        // its streak, as long as it falls in a sixtieth of a second
static const float kDropWidth = 0.002;          // meters

float2 hash2(float2 p)
{
    p = frac(p * float2(0.1031, 0.1030));
    p += dot(p, p.yx + 33.33);
    return frac((p.xx + p.yx) * p.yx);
}

float4 main(float4 position : SV_Position) : SV_Target0
{
    int2 pixel = int2(position.xy);
    float3 color = scene.Load(int3(pixel, 0));
    float2 uv = position.xy * render_size.zw;
    float3x3 to_world = transpose((float3x3)world_to_view);
    float3 dir = normalize(mul(to_world, view_position(uv, 1)));
    float scene_depth = depth.Load(int3(pixel, 0));
    float scene_meters = scene_depth > 0 ? length(view_position(uv, scene_depth)) : 1e9;
    float flat = length(dir.xz);
    if (flat < 0.05 || rain_amount <= 0)
        return float4(color, 1);
    float azimuth = atan2(dir.z, dir.x);
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
        // The layer's cell: drops a few centimeters apart near, wider far, where more of them fall in one pixel.
        float cell_w = 0.06 * sqrt(radius), cell_h = kStreak * 2.5;
        float height = dir.y * t;
        float across = azimuth * radius + height * slant;
        float down = height + kFall * rain_time + i * 7.31;
        float2 q = float2(across / cell_w, down / cell_h);
        float2 c = floor(q), f = q - c;
        float2 r = hash2(c + i * 41.7);
        if (r.x > rain_amount)
            continue;
        // The streak's width on screen: its own, or a pixel's, with its light spread over that.
        float pixel_m = t / max(projection_scale, 1);
        float width = max(kDropWidth, 1.5 * pixel_m), share = sqrt(kDropWidth / width);
        float dx = abs(f.x - r.y) * cell_w;
        float along = f.y * cell_h;
        float taper = saturate(along / kStreak) * saturate((kStreak * 1.2 - along) / (kStreak * 0.2));
        cover += saturate(1 - dx / (width * 0.75)) * taper * share * saturate(1 - radius / 30);
    }
    // A drop bends the bright sky above toward the eye, a few times the light that reaches the air around it.
    cover = saturate(cover);
    float3 drop = sky_color * 4;
    return float4(lerp(color, drop, cover * 0.6), 1);
}
