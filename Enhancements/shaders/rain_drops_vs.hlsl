// Rain's drops, one instanced draw over rain_ps.hlsl's output: each instance is a cell of one of rain_layers.hlsli's
// layers and draws the drop that falls through it as a streak, the stretch it falls in kShutter, on a quad facing the
// camera. The cells are fixed in the world, and a drop is a pure function of its cell and the game's clock, so the drops do
// not move with the camera, and a paused game and a screenshot's frames see the same rain. The cells' columns fall each at
// a speed of its own and drift with the wind, so a column keeps one drop a cell and a drop leaves the drawn cells only at a
// layer's edge, where it hands over to the next layer.
//
// A drop is clear water, so most of what it shows is what lies behind it, which rain_drops_ps.hlsl adds. Its own light,
// figured here once a drop, is the main light scattered forward through it, where the sun atlas lights it, a little of
// the sky it reflects, and lightning's flash. Seen away from the main light, rain is faint; toward the sun or the moon,
// within a few tens of degrees, its drops glow.
#include "sun.hlsli"
#include "scatter.hlsli"
#include "rain.hlsli"
#include "rain_layers.hlsli"

Texture2D<float> atlas : register(t5);              // mirror of the game's sun atlas, copied at its shadow-mask draw
SamplerComparisonState lit_compare : register(s2);  // bilinear, GREATER_EQUAL: lit where the point is at least as near
                                                    // the sun as what the atlas holds

static const float kWind = 4;                       // meters per second sideways on the wind: a slant of one in two
static const float2 kDiameter = float2(0.0015, 0.0025);     // meters, a drop's least and most
static const float2 kSpeed = float2(7, 9);          // meters per second, those drops' terminal speeds
// The share of a pixel a streak takes where it is a pixel or more wide. A drop covers each point of its streak only for
// its own length over the streak's, under 1%, but a camera's streaks show far more than that: 3% left the rain unseen.
static const float kOpacity = 0.25;
// A farther layer's drop stands for this power of its cell over the nearest layer's cell. 3 would keep the rain's mean
// light, and leave far rain as sparse bright lines; the mist draws the rain far off.
static const float kFarBoost = 1.5;
static const float kMaxCover = 0.5;
static const float kHandOver = 0.25;                // the outer share of a layer's reach over which the next takes its drops
static const float2 kNear = float2(0.35, 0.8);      // meters: drops this near are out of focus, and fade out
// The light a drop scatters over what reaches its face, diffraction's forward peak included, into two forward
// Henyey-Greenstein lobes: the narrow one the glow right around the light, the wide one the glow tens of degrees off.
static const float kScatter = 2;
static const float2 kLobes = float2(0.9, 0.6);
static const float kNarrowShare = 0.5;
static const float kReflect = 0.06;                 // the sky a drop reflects, averaged over its face
// A drop is a wide-angle lens: most of what it bends toward the eye comes from the bright sky above, so against darker
// scenery it shows as a light streak, and against the sky it nearly vanishes.
static const float kSkyThrough = 1.2;
static const float3 kFlashTint = float3(0.85, 0.92, 1.1);   // as rain_ps.hlsl's

uint3 pcg3d(uint3 v)
{
    v = v * 1664525u + 1013904223u;
    v.x += v.y * v.z;
    v.y += v.z * v.x;
    v.z += v.x * v.y;
    v ^= v >> 16u;
    v.x += v.y * v.z;
    v.y += v.z * v.x;
    v.z += v.x * v.y;
    return v;
}

float3 unit3(uint3 v) { return float3(v >> 8u) / 16777216.0; }

// The main light's visibility at a point rel meters from the camera along the world's axes, from the game's sun atlas:
// lit past its shadow distance, outside its cascades, and when run_rain bound no atlas, which it marks with no cascades.
float sun_visibility(float3 rel, float forward_meters)
{
    if (sun_cascades == 0 || forward_meters > game_b0[10].x)
        return 1;
    float3 p = game_b1[5].xyz + rel;
    float edge;
    int c = cascade_of(p, 0, edge);
    if (c < 0)
        return 1;
    CascadeTransform t = cascade_transform(c);
    float4 tile = cascade_tile(c);
    float3 q = p - t.origin;
    float4 a = q.x * t.m0 + q.y * t.m1 + q.z * t.m2 + t.m3;
    // Raised by the atlas's 16-bit step toward lit, as receiver_depth is.
    return atlas.SampleCmpLevelZero(lit_compare, clamp(a.xy / a.w, tile.xy, tile.zw), a.z / a.w + 2e-5);
}

RainStreak main(uint vertex : SV_VertexID, uint instance : SV_InstanceID)
{
    RainStreak o;
    o.position = float4(2, 2, 0, 1);
    o.at = 0;
    o.inverse_meters = 0;
    o.shape = 0;
    o.light = 0;

    int layer = 0, first = 0, n = kRainCells[0], start = 0;
    float reach = kRainReach[0], inner = 0, previous = 0;
    [unroll] for (int i = 0; i < kRainLayers; i++)
    {
        if (int(instance) >= start)
        {
            layer = i;
            first = start;
            n = kRainCells[i];
            reach = kRainReach[i];
            inner = previous;
        }
        previous = kRainReach[i];
        start += kRainCells[i] * kRainCells[i] * kRainCells[i];
    }
    if (int(instance) >= start || rain_amount <= 0)
        return o;
    int local = int(instance) - first;
    int3 slot = int3(local % n, (local / n) % n, local / (n * n));
    // The layer's cells cover reach meters from the camera along each axis wherever the camera lies in its cell and the
    // drops in theirs: n / 2 - 1 cells each way.
    float cell = reach / (n / 2 - 1);
    float nearest_cell = kRainReach[0] / (kRainCells[0] / 2 - 1);
    float3 camera = mul(transpose((float3x3)world_to_view), -float3(world_to_view[0].w, world_to_view[1].w, world_to_view[2].w));
    float3 anchor = floor(camera / cell);
    float3 camera_in_cell = camera / cell - anchor;

    // The slot's column in the frame that drifts with the wind, its speed, and its row in the frame that falls with it.
    float2 wind = float2(cos(rain_wind), sin(rain_wind)) * kWind;
    float2 drift = wind * rain_time / cell;
    uint salt = uint(layer) * 0x9E3779B9u;
    int2 column = int2(anchor.xz) - n / 2 + slot.xz - int2(floor(drift));
    float3 c = unit3(pcg3d(uint3(asuint(column.x), salt, asuint(column.y))));
    float speed = lerp(kSpeed.x, kSpeed.y, c.x);
    float fall = speed * rain_time / cell;
    int row = int(anchor.y) - n / 2 + slot.y + int(floor(fall));
    uint3 h = pcg3d(uint3(asuint(column.x), asuint(row) ^ salt, asuint(column.y)));
    float3 r = unit3(h), r2 = unit3(pcg3d(h + 0x2545F491u));
    // Where the drop is from the camera, from small numbers: the cell's place and the drop's in it.
    float3 rel = (float3(slot) - float(n / 2) - camera_in_cell + r + float3(frac(drift.x), -frac(fall), frac(drift.y))) * cell;

    // Within the inner layer's reach that layer draws the drops; over the outer kHandOver of each reach the two share them.
    float span = max(abs(rel.x), max(abs(rel.y), abs(rel.z)));
    float weight = 1 - smoothstep(reach * (1 - kHandOver), reach, span);
    if (layer > 0)
        weight *= smoothstep(inner * (1 - kHandOver), inner, span);
    float live = saturate(rain_amount * rain_gust(atan2(rel.z, rel.x)));
    if (weight <= 0 || r2.x >= live)
        return o;

    float3 velocity = float3(wind.x, -speed, wind.y);
    float3x3 rotation = (float3x3)world_to_view;
    float3 head = mul(rotation, rel), tail = mul(rotation, rel - velocity * kShutter);
    if (max(head.z, tail.z) > -kNear.x)
        return o;
    float2 a = stored_of(head) * render_size.xy, b = stored_of(tail) * render_size.xy;
    float meters = -0.5 * (head.z + tail.z);
    // Drawn at least a pixel wide, with its light spread over that.
    float diameter = lerp(kDiameter.x, kDiameter.y, r2.y);
    float width = diameter * projection_scale / meters, drawn = max(width, 1);
    float length_px = length(b - a);
    float2 along = length_px > 1e-3 ? (b - a) / length_px : float2(0, 1);
    float2 across = float2(-along.y, along.x);
    // A pixel past each end and each side, for the pixel shader's edges.
    float s = (vertex & 1u) != 0 ? length_px + 1 : -1;
    float t = (vertex & 2u) != 0 ? 0.5 * drawn + 1 : -0.5 * drawn - 1;
    float2 corner = a + along * s + across * t;
    o.position = float4(corner.x * render_size.z * 2 - 1, 1 - corner.y * render_size.w * 2, 0, 1);
    o.at = float2(s, t);
    o.inverse_meters = (vertex & 1u) != 0 ? -1 / tail.z : -1 / head.z;
    o.shape = float4(length_px, drawn, across);

    float3 light = (kReflect + kSkyThrough) * sky_color + 2 * rain_flash * kFlashTint * sky_color;
    if (dot(toward_sun, toward_sun) > 0)
    {
        float3 middle = rel - 0.5 * velocity * kShutter;
        float cosine = dot(normalize(middle), toward_sun);
        float phase = lerp(henyey_greenstein(cosine, kLobes.y), henyey_greenstein(cosine, kLobes.x), kNarrowShare);
        // The game lights a white surface facing the sun with sun_color, so the sun's irradiance is pi times it.
        light += kPi * sun_color * kScatter * phase * sun_visibility(middle, meters);
    }
    // Thinner than the pixel it is drawn across: its light spreads over the pixel, kept brighter than its share, as the old
    // rain's was, so a far drop still reads.
    float cover = min(kOpacity * sqrt(width / drawn) * pow(cell / nearest_cell, kFarBoost), kMaxCover);
    o.light = float4(light, cover * weight * smoothstep(kNear.x, kNear.y, meters));
    return o;
}
