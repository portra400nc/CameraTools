// The sun-shadow pass's first draw: raw sun visibility into our render-size scratch, drawn again from the game's own
// shadow atlas and cascades with soft edges (PCSS): a blocker search finds how far above a pixel its casters are, and
// the penumbra widens with that distance. Each pixel turns its taps by the 4x4 pattern, so the raw result is noisy;
// sun_shadows_ps.hlsl's 4x4 blur averages it away. Pixels that are not world, lie past the shadow distance or outside
// every cascade keep the game's value.
#include "gbuffer.hlsli"

Texture2D<float2> shadow_mask : register(t0);       // mirror of the game's shadow mask: .x sun visibility
Texture2D<float> atlas : register(t5);              // mirror of the game's sun atlas, copied at its shadow-mask draw
SamplerComparisonState lit_compare : register(s2);  // bilinear, GREATER_EQUAL: lit where the receiver is at least as
                                                    // near the sun as what the atlas holds

// The game's pixel shader constants b0 to b3 at its shadow-mask draw, copied there. core/sun.h names the rows.
cbuffer GameSun0 : register(b1) { float4 game_b0[48]; };
cbuffer GameSun1 : register(b2) { float4 game_b1[12]; };
cbuffer GameSun2 : register(b3) { float4 game_b2[22]; };
cbuffer GameSun3 : register(b4) { float4 game_b3[57]; };

static const int kMaxCascades = 8;
static const int kTaps = 16;
static const float kMaxRadiusTexels = 24;   // the widest penumbra's radius, in atlas texels
static const float kSearchDistance = 16;    // meters: the blocker search reaches far enough for casters this far up
static const float kBlend = 0.1;            // the outer tenth of each cascade's radius fades into the next cascade
// The receiver plane's steepest rise toward the sun per meter across. Steeper planes, extended over a wide penumbra,
// leave the surface and shadow it with whatever lies beyond: over capture 111824, a cap of 8 shadowed 0.9% of the pixels
// the game lights fully, and 2 shadowed 0.001%.
static const float kMaxSlope = 2;

// A stored pixel in the game's view space. The game's constants already account for its targets being stored upside
// down, so v counts texture rows from the top, and flip plays no part.
float3 game_view(int2 pixel)
{
    float2 uv = (pixel + 0.5) * render_size.zw;
    float3 ndc = float3(uv * 2 - 1, 1 - 2 * depth.Load(int3(pixel, 0)));
    float4 view = ndc.x * game_b2[10] + ndc.y * game_b2[11] + ndc.z * game_b2[12] + game_b2[13];
    return view.xyz / view.w;
}

// The world position the game's shadow shaders compute for the same pixel, so it lands on the atlas exactly.
float3 game_world(float3 view) { return game_b1[5].xyz + view.x * game_b2[18].xyz + view.y * game_b2[19].xyz - view.z * game_b2[20].xyz; }

float3 game_world_at(int2 pixel) { return game_world(game_view(clamp(pixel, 0, int2(render_size.xy) - 1))); }

// The surface's normal from the depth buffer, across the neighbor nearer in depth on each axis so an edge does not bend
// it. The atlas holds geometry, which the geometric normal predicts better than the G-buffer's. 0 when degenerate.
float3 receiver_normal(int2 pixel, float3 p)
{
    float3 right = game_world_at(pixel + int2(1, 0)) - p, left = p - game_world_at(pixel - int2(1, 0));
    float3 down = game_world_at(pixel + int2(0, 1)) - p, up = p - game_world_at(pixel - int2(0, 1));
    float3 across = dot(left, left) == 0 || (dot(right, right) > 0 && dot(right, right) < dot(left, left)) ? right : left;
    float3 along = dot(up, up) == 0 || (dot(down, down) > 0 && dot(down, down) < dot(up, up)) ? down : up;
    float3 n = cross(across, along);
    return dot(n, n) > 1e-20 ? normalize(n) : 0;
}

// The first cascade from first on whose sphere holds p, as the game picks them; -1 when none does. edge is how far p
// lies into that sphere's outer kBlend of radius, 0 to 1.
int cascade_of(float3 p, int first, out float edge)
{
    int found = -1;
    edge = 0;
    [unroll] for (int i = kMaxCascades - 1; i >= 0; i--)
    {
        float3 d = p - game_b3[i].xyz;
        float f = dot(d, d) / game_b3[i].w;
        if (i >= first && i < int(sun_cascades) && f < 1)
        {
            found = i;
            edge = saturate((sqrt(f) - (1 - kBlend)) / kBlend);
        }
    }
    return found;
}

struct Receiver
{
    float3 atlas;           // u and v in the whole atlas, and depth, larger toward the sun
    float2 lo, hi;          // the cascade's tile, a texel in from each edge, so taps never read a neighbor
    float2 uv_per_meter;    // across the light, along u and along v
    float meters_per_depth;
    float2 plane;           // the receiver plane's depth change per unit of u and of v
};

Receiver project(int c, float3 p, float3 n)
{
    // Picked by an unrolled loop, not game_b3[15 + 4 * c]: vkd3d-compiler declares a dynamically indexed buffer as
    // immediately indexed, which the bytecode does not allow.
    float4 m0 = 0, m1 = 0, m2 = 0, m3 = 0, origin = 0;
    [unroll] for (int i = 0; i < kMaxCascades; i++)
    {
        if (i == c)
        {
            m0 = game_b3[15 + 4 * i];
            m1 = game_b3[16 + 4 * i];
            m2 = game_b3[17 + 4 * i];
            m3 = game_b3[18 + 4 * i];
            origin = game_b3[49 + i];
        }
    }
    float3 q = p - origin.xyz;
    float4 a = q.x * m0 + q.y * m1 + q.z * m2 + m3;
    float3 gu = float3(m0.x, m1.x, m2.x), gv = float3(m0.y, m1.y, m2.y), gz = float3(m0.z, m1.z, m2.z);
    Receiver r;
    r.atlas = a.xyz / a.w;
    r.uv_per_meter = float2(length(gu), length(gv));
    r.meters_per_depth = 1 / length(gz);
    float2 grid = float2(sun_columns, sun_rows);
    float2 tile = float2(uint(c) % sun_columns, uint(c) / sun_columns);
    r.lo = tile / grid + game_b0[0].xy;
    r.hi = (tile + 1) / grid - game_b0[0].xy;
    // A surface lit at a slant rises toward the sun across the light. Predicting its depth at each tap keeps a wide
    // filter from shadowing the surface with itself.
    float facing = dot(n, normalize(gz));
    float2 rise = -float2(dot(n, normalize(gu)), dot(n, normalize(gv))) / (abs(facing) < 1e-4 ? 1e-4 : facing);
    r.plane = clamp(rise, -kMaxSlope, kMaxSlope) / (r.meters_per_depth * r.uv_per_meter);
    return r;
}

// Tap i of a Vogel disc of radius meters, turned by turn of a whole turn, inside the cascade's tile.
float2 tap(Receiver r, int i, float turn, float radius)
{
    float angle = i * 2.39996323 + turn * 6.2831853;
    float2 offset = sqrt((i + 0.5) / kTaps) * float2(cos(angle), sin(angle)) * radius * r.uv_per_meter;
    return clamp(r.atlas.xy + offset, r.lo, r.hi);
}

// The receiver plane's depth at uv, raised by the atlas's 16-bit step toward lit. The game's own bias, a thousandth of
// a cascade's depth range in capture 111824, is already in the matrices.
float receiver_depth(Receiver r, float2 uv) { return r.atlas.z + dot(r.plane, uv - r.atlas.xy) + 2e-5; }

float soft_visibility(Receiver r, float turn)
{
    float widest = kMaxRadiusTexels * game_b0[0].x / r.uv_per_meter.x;
    float search = min(0.5 * max(sun_light_size * kSearchDistance, sun_min_penumbra), widest);
    float blockers = 0, above = 0;
    [unroll] for (int i = 0; i < kTaps; i++)
    {
        float2 uv = tap(r, i, turn, search);
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
        float2 uv = tap(r, j, turn, radius);
        lit += atlas.SampleCmpLevelZero(lit_compare, uv, receiver_depth(r, uv));
    }
    return lit / kTaps;
}

float main(float4 position : SV_Position) : SV_Target0
{
    int2 pixel = int2(position.xy);
    float game = shadow_mask.Load(int3(pixel, 0)).x;
    if (!is_world(stencil.Load(int3(pixel, 0)).g))
        return game;
    float3 view = game_view(pixel);
    if (-view.z >= game_b0[10].x)
        return game;                        // past the game's shadow distance
    float3 p = game_world(view);
    float edge, next_edge;
    int c = cascade_of(p, 0, edge);
    if (c < 0)
        return game;                        // past the cascades, where the game reads its far map
    // In a cascade's outer band, a growing share of pixels take the next cascade; the blur turns the share into a fade.
    if (edge > pattern(pixel.yx) && cascade_of(p, c + 1, next_edge) == c + 1)
        c++;
    float visibility = soft_visibility(project(c, p, receiver_normal(pixel, p)), pattern(pixel));
    return lerp(1, visibility, sun_strength);
}
