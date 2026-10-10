// The debug composite, drawn over the back buffer with the view's name in the top-left corner. One branch per Decode in
// core/plan.h.
#include "common.hlsli"

Texture2D<float4> image : register(t0);     // the shown snapshot, read through a non-sRGB view so stored bytes show as is
Texture2D<uint2> stencil : register(t1);    // the snapshot's stencil plane, bound for the stencil view only

static const uint DECODE_RGB = 0, DECODE_GRAY = 1, DECODE_MATERIAL = 2, DECODE_DEPTH = 3, DECODE_STENCIL = 4, DECODE_HDR = 5,
    DECODE_MISSING = 6;

// 5x7 glyphs in encode_label's order: space, A-Z, 0-9, '-', '?'. Bit row * 5 + column, column 0 on the left; x holds
// rows 0-3, y rows 4-6.
static const uint2 font[39] = {
    uint2(0x00000, 0x0000), uint2(0xFC62E, 0x4631), uint2(0x7C62F, 0x3E31), uint2(0x0862E, 0x3A21),  // space A B C
    uint2(0x8C527, 0x1D31), uint2(0x7843F, 0x7C21), uint2(0x7843F, 0x0421), uint2(0xE862E, 0x7A31),  // D E F G
    uint2(0xFC631, 0x4631), uint2(0x2108E, 0x3884), uint2(0x4211C, 0x1928), uint2(0x19531, 0x4525),  // H I J K
    uint2(0x08421, 0x7C21), uint2(0xAD771, 0x4631), uint2(0xACE31, 0x4639), uint2(0x8C62E, 0x3A31),  // L M N O
    uint2(0x7C62F, 0x0421), uint2(0x8C62E, 0x5935), uint2(0x7C62F, 0x4525), uint2(0x7043E, 0x3E10),  // P Q R S
    uint2(0x2109F, 0x1084), uint2(0x8C631, 0x3A31), uint2(0x8C631, 0x1151), uint2(0xAC631, 0x2AB5),  // T U V W
    uint2(0x22A31, 0x462A), uint2(0x22A31, 0x1084), uint2(0x2221F, 0x7C22), uint2(0xAE62E, 0x3A33),  // X Y Z 0
    uint2(0x210C4, 0x3884), uint2(0x4422E, 0x7C44), uint2(0x4111F, 0x3A30), uint2(0x4A988, 0x211F),  // 1 2 3 4
    uint2(0x83C3F, 0x3A30), uint2(0x7844C, 0x3A31), uint2(0x2221F, 0x0842), uint2(0x7462E, 0x3A31),  // 5 6 7 8
    uint2(0xF462E, 0x1910), uint2(0xF8000, 0x0000), uint2(0x4422E, 0x1004),                          // 9 - ?
};

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

float3 view_color(float4 position, float2 uv)
{
    // The entry was not found this frame: dark magenta stripes, unlike anything the game draws.
    if (decode == DECODE_MISSING)
        return frac((position.x + position.y) / 64) < 0.5 ? float3(0.45, 0, 0.45) : float3(0.1, 0, 0.1);

    float2 st = stored_uv(uv);
    float4 v = image.SampleLevel(point_clamp, st, 0);
    uint width, height;
    image.GetDimensions(width, height);

    if (decode == DECODE_RGB)
        return v.rgb;
    if (decode == DECODE_GRAY)
        return v.rrr;
    if (decode == DECODE_MATERIAL)
        return palette(uint(v.r * 255 + 0.5) & 0x3Fu);
    if (decode == DECODE_DEPTH)
        return sqrt(saturate(v.r * 20)).xxx;     // reversed Z: near is bright, sky (0) black
    if (decode == DECODE_STENCIL)
        return stencil_color(stencil.Load(int3(min(int2(st * float2(width, height)), int2(width, height) - 1), 0)).g);
    return pow(v.rgb / (1 + v.rgb), 1 / 2.2);   // HDR: Reinhard at exposure 1, then display gamma
}

// White glyphs on a dark box, 2 font pixels in from the corner with 2 of padding. A font pixel is 3 screen pixels at
// 800 lines, so the letters are 21 pixels tall on the Deck and 56 at 2160.
float3 with_label(float3 color, float2 position)
{
    float scale = max(1, round(render_size.y * 3 / 800));
    int2 cell = int2(floor(position / scale)) - 2;
    if (any(cell < 0) || cell.x >= int(label_length * 6 + 3) || cell.y >= 11)
        return color;
    int2 glyph_cell = cell - 2;
    uint i = uint(glyph_cell.x) / 6, column = uint(glyph_cell.x) % 6;
    if (any(glyph_cell < 0) || glyph_cell.y >= 7 || column >= 5 || i >= label_length)
        return color * 0.3;
    uint4 words = i < 16 ? label[0] : label[1];  // not label[i >> 4]: the composite's cbuffer stays immediately indexed
    uint glyph = (words[(i >> 2) & 3] >> ((i & 3) * 8)) & 0xFFu;
    uint2 rows = font[min(glyph, 38u)];
    uint bit = uint(glyph_cell.y) * 5 + column;
    bool lit = ((bit < 20 ? rows.x >> bit : rows.y >> (bit - 20)) & 1u) != 0;
    return lit ? 1 : color * 0.3;
}

float4 main(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target0
{
    return float4(with_label(view_color(position, uv), position.xy), 1);
}
