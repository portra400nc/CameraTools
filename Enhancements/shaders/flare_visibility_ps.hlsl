// The lens flare's source's visibility, one texel at the bloom step, eased from the last frame's. On the picture, the share
// of a small disc around the sun where the depth buffer holds sky; off it, the share of a small disc around the camera,
// across the light, that the game's sun shadow atlas lights; near the picture's edge, a mix by the share of the disc on
// it. The manual source is always visible, and a sun behind the camera never is.
#include "sun.hlsli"

Texture2D<float> atlas : register(t5);              // mirror of the game's sun atlas, copied at its shadow-mask draw
Texture2D<float> previous : register(t6);           // the last frame's visibility
SamplerComparisonState lit_compare : register(s2);  // bilinear, GREATER_EQUAL: lit where the point is at least as near

static const int kScreenTaps = 32;
static const int kAtlasTaps = 12;
static const float kAtlasRadius = 0.25;     // meters around the camera, about a lens hood's reach

float2 vogel(int i, int count)
{
    float angle = i * 2.39996323;
    return sqrt((i + 0.5) / count) * float2(cos(angle), sin(angle));
}

float atlas_visibility()
{
    if (flare_atlas == 0)
        return 1;
    float3 camera = game_b1[5].xyz;
    float edge;
    int c = cascade_of(camera, 0, edge);
    if (c < 0)
        return 1;
    CascadeTransform t = cascade_transform(c);
    float4 tile = cascade_tile(c);
    float3 across = normalize(cross(toward_sun, abs(toward_sun.y) < 0.9 ? float3(0, 1, 0) : float3(1, 0, 0)));
    float3 up = cross(toward_sun, across);
    float lit = 0;
    [unroll] for (int i = 0; i < kAtlasTaps; i++)
    {
        float2 o = vogel(i, kAtlasTaps) * kAtlasRadius;
        float3 q = camera + across * o.x + up * o.y - t.origin;
        float4 a = q.x * t.m0 + q.y * t.m1 + q.z * t.m2 + t.m3;
        lit += atlas.SampleCmpLevelZero(lit_compare, clamp(a.xy / a.w, tile.xy, tile.zw), a.z / a.w + 2e-5);
    }
    return lit / kAtlasTaps;
}

float main(float4 position : SV_Position) : SV_Target0
{
    float target = flare_mode == 1 ? 1 : 0;
    if (flare_mode == 0)
    {
        float aspect = render_size.x * render_size.w;
        float sky = 0, on = 0;
        [unroll] for (int i = 0; i < kScreenTaps; i++)
        {
            float2 o = vogel(i, kScreenTaps) * flare_disc;
            float2 uv = flare_sun_uv + float2(o.x / aspect, o.y);
            if (all(uv >= 0) && all(uv < 1))
            {
                float d = depth.Load(int3(stored_uv(uv) * render_size.xy, 0));
                sky += d <= 0 ? 1 : 0;
                on += 1;
            }
        }
        float off = atlas_visibility();
        target = on > 0 ? lerp(off, sky / on, on / kScreenTaps) : off;
    }
    return lerp(previous.Load(int3(0, 0, 0)), target, flare_ease);
}
