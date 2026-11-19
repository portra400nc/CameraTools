// Surface detail: one draw writes the G-buffer's normals and albedo with a tiling detail map blended in, into mirrors of
// them that are copied back over the game's before its lighting reads them. Only pixels a rock draw wrote this frame take
// it: the mask holds the rock's world position and view depth as its own draw computed them (surface_mask.hlsli), so the
// detail is fixed to the rock whatever the camera does. A pixel whose depth now lies nearer than the mask's was covered by
// a later draw and keeps its stored values exactly, as does every other pixel.
#include "gbuffer.hlsli"

Texture2D<float4> albedo : register(t5);    // mirror of rt1, sRGB-encoded as stored
Texture2D<float4> mask : register(t6);      // the rock's world position and view depth this frame, zero elsewhere
Texture2D<float4> detail : register(t7);    // rg the tangent-space normal's xy, b the color's luminance around 0.5, a roughness
SamplerState anisotropic_wrap : register(s4);

cbuffer Surface : register(b1)
{
    float strength;     // 0 to 1 and past it
    float tile;         // meters one repeat of the detail map spans
    float fade_near;    // meters: full strength up to here
    float fade_far;     // meters: none past here
};

struct GBuffer
{
    float4 normal : SV_Target0;
    float4 albedo : SV_Target1;
};

float3 srgb_to_linear(float3 c) { return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4); }
float3 linear_to_srgb(float3 c) { return c <= 0.0031308 ? c * 12.92 : 1.055 * pow(c, 1 / 2.4) - 0.055; }

static const float kFarScale = 7;     // the coarse octave's repeat over the fine one's
static const float kFarFade = 300;    // meters: no coarse octave past here

float3 tangent_normal(float4 texel)
{
    float2 xy = texel.rg * 2 - 1;
    return float3(xy, sqrt(saturate(1 - dot(xy, xy))));
}

// The detail map's triplanar normal on a surface of world normal n with plane weights w, at map coordinates p, blended in
// by whiteout; shade the map's luminance there.
float3 detail_normal(float3 n, float3 w, float3 p, out float shade)
{
    float4 dx = detail.Sample(anisotropic_wrap, p.zy), dy = detail.Sample(anisotropic_wrap, p.xz), dz = detail.Sample(anisotropic_wrap, p.xy);
    float3 tx = tangent_normal(dx), ty = tangent_normal(dy), tz = tangent_normal(dz);
    tx = float3(tx.xy + n.zy, abs(tx.z) * n.x);
    ty = float3(ty.xy + n.xz, abs(ty.z) * n.y);
    tz = float3(tz.xy + n.xy, abs(tz.z) * n.z);
    shade = dx.b * w.x + dy.b * w.y + dz.b * w.z;
    return normalize(tx.zyx * w.x + ty.xzy * w.y + tz * w.z);
}

GBuffer main(float4 position : SV_Position)
{
    int2 pixel = int2(position.xy);
    GBuffer stored;
    stored.normal = normals.Load(int3(pixel, 0));
    stored.albedo = albedo.Load(int3(pixel, 0));
    float4 m = mask.Load(int3(pixel, 0));
    if (m.w <= 0)
        return stored;
    // The view depth the depth buffer holds now, against the rock's: a nearer one means a later draw covers the rock.
    // The projection only scales depth here, so the pushed camera's serves.
    float depth_now = -view_position_at(pixel).z;
    if (depth_now < m.w * 0.995 - 0.02)
        return stored;
    float distance = m.w;
    // Two octaves: the map at its own scale near the camera, and seven times larger, which carries the cliffs farther out.
    float near_share = strength * saturate((fade_far - distance) / max(fade_far - fade_near, 1e-3));
    float far_share = strength * saturate((kFarFade - distance) / (kFarFade - fade_near)) * 0.8;
    if (near_share <= 0 && far_share <= 0)
        return stored;
    float3 n = normalize(stored.normal.rgb * 2 - 1);
    float3 w = pow(abs(n), 4);
    w /= dot(w, 1);
    float3 p = m.xyz;
    float shade_near, shade_far;
    float3 detailed = detail_normal(n, w, p / tile, shade_near);
    detailed = normalize(lerp(n, detailed, saturate(near_share)));
    float3 coarse = detail_normal(detailed, w, p / (tile * kFarScale) + 0.37, shade_far);
    detailed = normalize(lerp(detailed, coarse, saturate(far_share)));
    GBuffer o;
    o.normal = float4(detailed * 0.5 + 0.5, stored.normal.a);
    // The color only darkens and lightens around the painted one: the map's luminance, centered on 0.5.
    float shade = 1 + (shade_near - 0.5) * 0.9 * near_share + (shade_far - 0.5) * 0.7 * far_share;
    float3 color = srgb_to_linear(stored.albedo.rgb) * max(shade, 0);
    o.albedo = float4(linear_to_srgb(saturate(color)), stored.albedo.a);
    return o;
}
