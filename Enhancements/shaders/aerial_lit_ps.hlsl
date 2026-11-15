// The physical sky's distance haze, its first draw at half size: the share of the air between the camera and a scene
// pixel that the main light reaches, from the game's sun shadow atlas, as shafts_ps.hlsl marches it. The air past the
// game's shadow distance counts as lit. sky_ps.hlsl lights only that share of the haze with the sun's or the moon's own
// light, so a hill with the sun behind it keeps the shade of the air in front of it.
#include "sun.hlsli"

Texture2D<float> atlas : register(t5);              // mirror of the game's sun atlas, copied at its shadow-mask draw
SamplerComparisonState lit_compare : register(s2);  // bilinear, GREATER_EQUAL: lit where the point is at least as near
                                                    // the sun as what the atlas holds

static const uint kSteps = 16;

float main(float4 position : SV_Position) : SV_Target0
{
    int2 q = int2(position.xy);
    // The four half-size pixels of a 2x2 block look through the four full-size pixels of their cells, as the shafts' do.
    int2 pixel = min(q * 2 + int2(q.y & 1, q.x & 1), int2(render_size.xy) - 1);
    float d = depth.Load(int3(pixel, 0));
    if (d <= 0)
        return 1;
    float3 view = game_view_at((pixel + 0.5) * render_size.zw, d);
    float3 origin = game_b1[5].xyz;
    float meters = length(view);
    float3 dir = normalize(game_world(view) - origin);
    float forward = dot(dir, game_b2[20].xyz);
    float shadowed = min(meters, forward > 0 ? game_b0[10].x / forward : meters);
    float2 spans[kMaxCascades];
    [unroll] for (int k = 0; k < kMaxCascades; k++)
    {
        float3 offset = origin - game_b3[k].xyz;
        float b = dot(offset, dir);
        float discriminant = b * b - dot(offset, offset) + game_b3[k].w;
        float half_chord = sqrt(max(discriminant, 0));
        spans[k] = k < int(sun_cascades) && discriminant > 0 ? float2(-b - half_chord, -b + half_chord) : float2(1, 0);
    }
    float lit = 0;
    int cascade = -1;
    float4 start = 0, per_meter = 0, tile = 0;
    [loop] for (uint i = 0; i < kSteps; i++)
    {
        float t = (i + 0.5) / kSteps * shadowed;
        float visibility = 1;
        int c = -1;
        [unroll] for (int j = kMaxCascades - 1; j >= 0; j--)
        {
            if (t >= spans[j].x && t < spans[j].y)
                c = j;
        }
        [branch] if (c >= 0)
        {
            [branch] if (c != cascade)
            {
                cascade = c;
                CascadeTransform x = cascade_transform(c);
                float3 o = origin - x.origin;
                start = o.x * x.m0 + o.y * x.m1 + o.z * x.m2 + x.m3;
                per_meter = dir.x * x.m0 + dir.y * x.m1 + dir.z * x.m2;
                tile = cascade_tile(c);
            }
            float4 a = start + t * per_meter;
            visibility = atlas.SampleCmpLevelZero(lit_compare, clamp(a.xy / a.w, tile.xy, tile.zw), a.z / a.w + 2e-5);
        }
        lit += visibility;
    }
    // The haze's air is spread evenly enough near the ground that its light grows with the meters lit.
    return (lit / kSteps * shadowed + (meters - shadowed)) / max(meters, 1e-3);
}
