// The debug composite, drawn over the back buffer. One branch per Decode in core/plan.h.
#include "common.hlsli"

Texture2D<float4> image : register(t0);     // the shown snapshot, read through a non-sRGB view so stored bytes show as is
Texture2D<uint2> stencil : register(t1);    // the snapshot's stencil plane, bound for the stencil view only

static const uint DECODE_RGB = 0, DECODE_GRAY = 1, DECODE_MATERIAL = 2, DECODE_DEPTH = 3, DECODE_STENCIL = 4, DECODE_HDR = 5,
    DECODE_MISSING = 6;

// Material IDs use the low six bits; a golden-ratio hue walk keeps neighbouring IDs apart.
float3 palette(uint i)
{
    if (i == 0)
        return 0;
    float hue = frac(i * 0.618034);
    return saturate(abs(frac(hue + float3(0, 2.0 / 3, 1.0 / 3)) * 6 - 3) - 1) * 0.8 + 0.2;
}

float3 stencil_color(uint s)
{
    // 0 sky black, 128 world grey, 129 grass green, 133 character magenta, 136 vegetation dark green,
    // 137 foliage light green, anything else red, so a new value stands out.
    if (s == 0) return 0;
    if (s == 128) return 0.5;
    if (s == 129) return float3(0.2, 0.7, 0.2);
    if (s == 133) return float3(0.9, 0.2, 0.9);
    if (s == 136) return float3(0.1, 0.4, 0.1);
    if (s == 137) return float3(0.5, 0.9, 0.4);
    return float3(1, 0, 0);
}

float4 main(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target0
{
    // The entry was not found this frame: dark magenta stripes, unlike anything the game draws.
    if (decode == DECODE_MISSING)
        return float4(frac((position.x + position.y) / 64) < 0.5 ? float3(0.45, 0, 0.45) : float3(0.1, 0, 0.1), 1);

    float2 st = stored_uv(uv);
    float4 v = image.SampleLevel(point_clamp, st, 0);
    uint width, height;
    image.GetDimensions(width, height);

    if (decode == DECODE_RGB)
        return float4(v.rgb, 1);
    if (decode == DECODE_GRAY)
        return float4(v.rrr, 1);
    if (decode == DECODE_MATERIAL)
        return float4(palette(uint(v.r * 255 + 0.5) & 0x3Fu), 1);
    if (decode == DECODE_DEPTH)
        return float4(sqrt(saturate(v.r * 20)).xxx, 1);     // reversed Z: near is bright, sky (0) black
    if (decode == DECODE_STENCIL)
        return float4(stencil_color(stencil.Load(int3(min(int2(st * float2(width, height)), int2(width, height) - 1), 0)).g), 1);
    return float4(pow(v.rgb / (1 + v.rgb), 1 / 2.2), 1);   // HDR: Reinhard at exposure 1, then display gamma
}
