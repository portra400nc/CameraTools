// The tonemap pass, drawn into a mirror of the game's tone map output in place of the game's draw, which the add-on
// skips: the HDR scene plus the game's final bloom through the game's color matrix and exposure, then a curve, encoded as
// the game encodes it, with luma in alpha and the game's dither. The pass after the tone map reads it as is.
#include "common.hlsli"

Texture2D<float3> scene : register(t0);     // mirror of the game's HDR scene
Texture2D<float3> bloom : register(t1);     // mirror of the game's final bloom, a quarter of the render size; unbound
                                            // when the tone map draw had none, so it adds nothing

// The skipped draw's own constants (its b0), copied each frame, or unbound when they could not be. Rows 20, 22 and 99 to
// 101 sit at the same place in every tone map shader variant the census found.
cbuffer GameTonemap : register(b1)
{
    float4 game[102];
};

static const float3 kLuma = float3(0.2126, 0.7152, 0.0722);

struct Grade
{
    float exposure;
    float bloom;        // the bloom texture's weight
    float3x3 color;     // rows: what red, green and blue each become
    float gamma;        // a power after the encode; 1 at default brightness
};

Grade game_grade()
{
    Grade g;
    g.exposure = game[22].z;
    g.bloom = game[22].y;
    g.color = float3x3(game[99].xyz, game[100].xyz, game[101].xyz);
    g.gamma = game[20].y;
    // Comparisons with NaN are false, so a NaN anywhere also falls back. The fallback is the census's 1920x1200 frame.
    bool known = g.exposure > 0 && g.exposure < 100 && g.bloom >= 0 && g.bloom < 100 && g.gamma > 0.1 && g.gamma < 10
        && all(abs(g.color[0]) < 100) && all(abs(g.color[1]) < 100) && all(abs(g.color[2]) < 100);
    if (!known)
    {
        g.exposure = 1;
        g.bloom = 0.75;
        g.color = float3x3(1, 0, 0, 0, 1, 0, 0, 0, 1);
        g.gamma = 1;
    }
    return g;
}

float3 game_filmic(float3 x)
{
    return saturate(x * (1.36 * x + 0.047) / (x * (0.93 * x + 0.56) + 0.14));
}

// AgX with Troy Sobotka's inset and outset matrices and a sixth-order fit of its contrast curve, as in Benjamin Wrensch's
// minimal AgX, then back to linear so every curve shares the game's encode.
float3 agx(float3 x)
{
    const float3x3 inset = float3x3(0.842479062253094, 0.0423282422610123, 0.0423756549057051,
        0.0784335999999992, 0.878468636469772, 0.0784336,
        0.0792237451477643, 0.0791661274605434, 0.879142973793104);
    const float3x3 outset = float3x3(1.19687900512017, -0.0528968517574562, -0.0529716355144438,
        -0.0980208811401368, 1.15190312990417, -0.0980434501171241,
        -0.0990297440797205, -0.0989611768448433, 1.15107367264116);
    const float min_ev = -12.47393, max_ev = 4.026069;
    x = mul(x, inset);
    x = (clamp(log2(max(x, 1e-10)), min_ev, max_ev) - min_ev) / (max_ev - min_ev);
    float3 x2 = x * x, x4 = x2 * x2;
    x = 15.5 * x4 * x2 - 40.14 * x4 * x + 31.96 * x4 - 6.868 * x2 * x + 0.4298 * x2 + 0.1191 * x - 0.00232;
    return pow(max(mul(x, outset), 0), 2.2);
}

// Khronos PBR Neutral.
float3 pbr_neutral(float3 color)
{
    const float start = 0.8 - 0.04, desaturation = 0.15, d = 1 - start;
    float x = min(color.r, min(color.g, color.b));
    color -= x < 0.08 ? x - 6.25 * x * x : 0.04;
    float peak = max(color.r, max(color.g, color.b));
    if (peak < start)
        return color;
    float new_peak = 1 - d * d / (peak + d - start);
    color *= new_peak / peak;
    float g = 1 - 1 / (desaturation * (peak - new_peak) + 1);
    return lerp(color, new_peak, g);
}

float4 main(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target0
{
    Grade g = game_grade();
    float3 hdr = scene.Load(int3(position.xy, 0)) + bloom.SampleLevel(linear_clamp, uv, 0) * (g.bloom * bloom_strength);
    float3 x = mul(hdr, g.color) * (g.exposure * exposure_scale);
    x = max(lerp(dot(x, kLuma), x, saturation), 0);
    x = 0.18 * pow(x / 0.18, contrast);
    float3 c = curve == 1 ? agx(x) : curve == 2 ? pbr_neutral(x) : game_filmic(x);
    float3 encoded = pow(max(pow(max(c, 0), 1 / 2.4) * 1.055 - 0.055, 0), g.gamma);
    float noise = frac(52.9829189 * frac(dot(position.xy, float2(0.06711056, 0.00583715))));
    return float4(encoded + (noise * 2 - 0.5) / 255, dot(encoded, kLuma));
}
