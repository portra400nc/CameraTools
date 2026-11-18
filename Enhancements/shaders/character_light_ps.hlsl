// The character light pass's last draw, at the bloom step before the traced lights: the world's light scales the game's
// on characters when it is on, from the trace's sun and sky where a traced shot has samples and from the game's sun atlas
// elsewhere, and CameraTools' lights light them as studio lamps. A character pixel keeps params.x of the game's light and takes each light's albedo-weighted
// key, a toon step or a soft falloff across its normal, behind character_shadow_ps.hlsl's visibility averaged over the 4x4
// window, and a rim where the light stands behind it. Writes the whole target, because the result is copied over the
// game's HDR scene whole.
#include "character_light.hlsli"

Texture2D<float3> scene : register(t0);     // mirror of the game's HDR scene
Texture2D<float4> shadow : register(t7);    // the first draw's raw visibility, a light a channel
Texture2D<float> sun : register(t8);        // character_sun_ps.hlsl's raw sun visibility
StructuredBuffer<float4> history : register(t9);        // the trace's: occlusion sum, sun sum, their counts
StructuredBuffer<float4> gather_hit : register(t10);    // the gather's hits' light summed
StructuredBuffer<float4> gather_sky : register(t11);    // the misses' sky summed, and the misses

cbuffer GatherApply : register(b2)
{
    float4 gather_apply;        // strength, ...
    uint4 gather_modes;
    float4 sky_harmonics[9];    // core/light.h's SkyMap::harmonics: the sky map's light on a surface of normal n over pi
};

// The trace's pixels are noisy, and the toon shading must survive them: the game already shades a body's sides, so the
// traced light scales a character evenly and only the sun's shade draws edges. The sun's visibility pools over a window
// kSunReach taps each way, a tap every kSunStep pixels at 720 lines (wider in proportion above), weighed by depth alone;
// the sky's over a window as wide as kSkyStep taps, with every facing alike, so it is about one value a body.
static const int kSunReach = 4;
static const float kSunStep = 2;
static const int kSkyReach = 4;
static const float kSkyStep = 10;
static const float kSkyTint = 0.5;

float3 sky_light(float3 n)
{
    return sky_harmonics[0].rgb + sky_harmonics[1].rgb * n.y + sky_harmonics[2].rgb * n.z + sky_harmonics[3].rgb * n.x + sky_harmonics[4].rgb * (n.x * n.y)
        + sky_harmonics[5].rgb * (n.y * n.z) + sky_harmonics[6].rgb * (3 * n.z * n.z - 1) + sky_harmonics[7].rgb * (n.x * n.z)
        + sky_harmonics[8].rgb * (n.x * n.x - n.y * n.y);
}

struct Traced
{
    float sun;          // the sun's visibility
    float3 sky;         // the hemisphere's light over the open sky's on this normal, per channel
    bool known;
};

// The pooled weight of a tap at q for a pixel at view depth z: 0 off a character, across a depth jump of more than
// tolerance times the distance, or outside the frame.
float tap_weight(int2 q, float z, float tolerance)
{
    if (any(q < 0) || any(q >= int2(render_size.xy)) || !is_character(stencil.Load(int3(q, 0)).g))
        return 0;
    return saturate(1 - abs(view_position_at(q).z - z) / (max(-z, 0.05) * tolerance));
}

// The traced sun's visibility and sky light at a character pixel, pooled from its neighbors' sums.
Traced traced_at(int2 pixel, float z)
{
    Traced t;
    t.known = false;
    t.sun = 1;
    t.sky = 1;
    float scale = render_size.y / 720;
    int stride = max(1, int(kSunStep * scale + 0.5));
    float4 h = 0;
    [loop] for (int y = -kSunReach; y <= kSunReach; y++)
    {
        [loop] for (int x = -kSunReach; x <= kSunReach; x++)
        {
            int2 q = pixel + int2(x, y) * stride;
            float weight = tap_weight(q, z, kDepthTolerance * 4);
            if (weight > 0)
                h += history[uint(q.y) * uint(render_size.x) + uint(q.x)] * weight;
        }
    }
    if (!(h.w > 0))
        return t;
    t.known = true;
    t.sun = h.y / h.w;
    stride = max(1, int(kSkyStep * scale + 0.5));
    float4 pooled = 0, hit = 0, seen = 0;
    float3 open_sky = 0;
    [loop] for (int v = -kSkyReach; v <= kSkyReach; v++)
    {
        [loop] for (int u = -kSkyReach; u <= kSkyReach; u++)
        {
            int2 q = pixel + int2(u, v) * stride;
            float weight = tap_weight(q, z, kDepthTolerance * 16);
            if (weight <= 0)
                continue;
            uint at = uint(q.y) * uint(render_size.x) + uint(q.x);
            float4 hq = history[at];
            pooled += hq * weight;
            if (traced.y > 0.5 && hq.z > 0)
            {
                hit += gather_hit[at] * weight;
                seen += gather_sky[at] * weight;
                open_sky += max(sky_light(normalize(normals.Load(int3(q, 0)).xyz * 2 - 1)), 1e-4) * hq.z * weight;
            }
        }
    }
    // The gathered light over what the open_sky sky would give the same normals, the sky's and the bounce's together.
    if (traced.y > 0.5 && pooled.z > 0)
        t.sky = clamp((seen.rgb + gather_apply.x * hit.rgb) / max(open_sky, 1e-4), 0, 4);
    else if (pooled.z > 0)
        t.sky = pooled.x / pooled.z;
    // At most the open sky's brightness, keeping its color: a strong bounce lifted a whole body past the game's look in
    // the open, clipped its lit side to white and warmed its cool shaded side, which flattened the toon shading.
    float bright = dot(t.sky, float3(0.2126, 0.7152, 0.0722));
    t.sky *= bright > 1 ? 1 / bright : 1;
    // Half the bounce's tint: the grass's green and the ground's warmth, brightened by Bounce strength, painted bodies too
    // warm against the world.
    t.sky = lerp(min(bright, 1).xxx, t.sky, kSkyTint);
    return t;
}

float3 srgb_to_linear(float3 c) { return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4); }

// The raw visibility over one whole period of pattern(), so a smooth surface averages its 16 offsets. Taps off the
// character, and across a depth jump or a sharp normal change, fade out, as gbuffer.hlsli's blurred() weighs them.
float4 shadow_at(int2 pixel, float z, float3 n, bool of_sun)
{
    float4 sum = 0;
    float total = 0;
    [unroll] for (int y = -2; y < 2; y++)
    {
        [unroll] for (int x = -2; x < 2; x++)
        {
            int2 q = pixel + int2(x, y);
            if (any(q < 0) || any(q >= int2(render_size.xy)) || !is_character(stencil.Load(int3(q, 0)).g))
                continue;
            float depth_weight = saturate(1 - abs(view_position_at(q).z - z) / (max(-z, 0.05) * kDepthTolerance));
            float facing = dot(n, view_normal_at(q)) * 0.5 + 0.5;
            float weight = depth_weight * facing * facing;
            sum += (of_sun ? sun.Load(int3(q, 0)).xxxx : shadow.Load(int3(q, 0))) * weight;
            total += weight;
        }
    }
    return total > 0 ? sum / total : 1;
}

float3 main(float4 position : SV_Position) : SV_Target0
{
    int2 pixel = int2(position.xy);
    float3 color = scene.Load(int3(pixel, 0));
    if (depth.Load(int3(pixel, 0)) <= 0 || !is_character(stencil.Load(int3(pixel, 0)).g))
        return color;
    float3 p = view_position_at(pixel);
    bool face = is_face(pixel);
    float3 n = character_normal(pixel, p, face);
    float3 v = normalize(-p);
    float4 seen = shadow_at(pixel, p.z, view_normal_at(pixel), false);
    // The world's light: the game's light on the character, scaled to the sun's and the sky's here.
    if (params.w > 0.5)
    {
        // A traced shot's sun and sky where the trace has samples here, else the sun's shade from the game's atlas.
        Traced t;
        t.known = false;
        if (traced.x > 0.5)
            t = traced_at(pixel, p.z);
        if (t.known)
            color *= (t.sun * world_sun.rgb + t.sky * world_sky.rgb) / world_game.rgb;
        else
        {
            float lit = world_sun.w > 0.5 ? shadow_at(pixel, p.z, view_normal_at(pixel), true).x : 1;
            color *= (lit * world_sun.rgb + world_sky.rgb) / world_game.rgb;
        }
    }
    float3 base = srgb_to_linear(albedo.Load(int3(pixel, 0)).rgb);
    float facing = 1 - saturate(dot(n, v));
    float fresnel = facing * sqrt(facing);
    float2 edge = n.xy / max(length(n.xy), 1e-4);
    float3 key = 0, rim = 0;
    [unroll] for (uint k = 0; k < 4; k++)
    {
        if (k >= light_info.x)
            break;
        float4 r = local_reach_of(k, p);
        if (r.w <= 0)
            continue;
        float3 l = r.xyz;
        float3 radiance = light_rows[k * 4u + 1u].xyz * r.w;
        float nl = dot(n, l);
        float shade = params.z > 0.5 ? pow(saturate((nl + 0.2) / 1.2), 1.5) : smoothstep(0.55, 0.65, nl * 0.5 + 0.5);
        key += radiance * shade * (face ? max(seen[k], 0.5) : seen[k]);
        // Edges that turn away from the camera glow when the light stands behind them, on the light's side of the silhouette.
        float behind = saturate(0.5 - 0.5 * dot(v, l));
        float side = length(l.xy) > 0.2 ? saturate(dot(edge, normalize(l.xy)) * 1.5 + 0.25) : 1;
        rim += radiance * fresnel * behind * side;
    }
    return color * params.x + base * key + params.y * rim * (0.25 + base);
}
