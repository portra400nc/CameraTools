// The sun's soft visibility from the game's sun atlas (PCSS): a blocker search finds how far above a receiver its casters
// are, and the penumbra widens with that distance. Shared by sun_ps.hlsl and character_sun_ps.hlsl, which bind the atlas
// at t5.
#include "sun.hlsli"

Texture2D<float> atlas : register(t5);              // mirror of the game's sun atlas, copied at its shadow-mask draw
SamplerComparisonState lit_compare : register(s2);  // bilinear, GREATER_EQUAL: lit where the receiver is at least as
                                                    // near the sun as what the atlas holds

static const int kTaps = 16;
static const float kMaxRadiusTexels = 24;   // the widest penumbra's radius, in atlas texels
static const float kSearchDistance = 16;    // meters: the blocker search reaches far enough for casters this far up

float soft_visibility(Receiver r, float turn)
{
    float widest = kMaxRadiusTexels * game_b0[0].x / r.uv_per_meter.x;
    float search = min(0.5 * max(sun_light_size * kSearchDistance, sun_min_penumbra), widest);
    float blockers = 0, above = 0;
    [unroll] for (int i = 0; i < kTaps; i++)
    {
        float2 uv = tap(r, i, kTaps, turn, search);
        float stored = atlas.SampleLevel(point_clamp, uv, 0);
        float receiver = receiver_depth(r, uv);
        if (stored > receiver)
        {
            blockers++;
            above += stored - receiver;
        }
    }
    if (blockers == 0)
        return 1;
    float distance = above / blockers * r.meters_per_depth;
    float radius = min(0.5 * max(sun_light_size * distance, sun_min_penumbra), widest);
    float lit = 0;
    [unroll] for (int j = 0; j < kTaps; j++)
    {
        float2 uv = tap(r, j, kTaps, turn, radius);
        lit += atlas.SampleCmpLevelZero(lit_compare, uv, receiver_depth(r, uv));
    }
    return lit / kTaps;
}
