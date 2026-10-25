// The depth of field on the display: the back buffer's display-referred color decoded to linear light, with the main
// depth beside it at the back buffer's size and upright, for dof.hlsli's passes, which run there with the constants of
// dof_display_constants (core/dof.h); and their result encoded back. Shared by dof_display_in_ps.hlsl,
// dof_display_out_ps.hlsl and, compiled as C++, core/dof.cpp. Include dof.cbuffer.hlsli first.
//
// A block's includer defines, before the include:
//   DOF_DISPLAY_IN:   float4 dof_load_back(int2 pixel)        the back buffer, as stored, clamped to its size
//                     float dof_load_main_depth(int2 pixel)   the main depth, reversed, as stored, clamped to its size
//   DOF_DISPLAY_OUT:  float3 dof_load_blurred(int2 pixel)     the composite's linear color
//                     float4 dof_load_display(int2 pixel)     dof_display_in's color, for the back buffer's alpha

// The sRGB curve, which an 8-bit or 10-bit back buffer holds whichever of its two views ReShade draws through: the game's
// tone map encodes with its power 1 / 2.4 part, and ReShade's sRGB view encodes the same values in hardware. Each is the
// other's inverse, so a pixel the depth of field leaves sharp comes back as the same level.
float dof_srgb_to_linear(float v) { return v <= 0.04045f ? v / 12.92f : pow((v + 0.055f) / 1.055f, 2.4f); }
float dof_linear_to_srgb(float c) { return c <= 0.0031308f ? c * 12.92f : 1.055f * pow(c, 1.0f / 2.4f) - 0.055f; }

float3 dof_decode_display(float3 v)
{
    if (dof_display.w == 0.0f)
        return v;
    return float3(dof_srgb_to_linear(saturate(v.x)), dof_srgb_to_linear(saturate(v.y)), dof_srgb_to_linear(saturate(v.z)));
}

float3 dof_encode_display(float3 c)
{
    if (dof_display.w == 0.0f)
        return c;
    return float3(dof_linear_to_srgb(saturate(c.x)), dof_linear_to_srgb(saturate(c.y)), dof_linear_to_srgb(saturate(c.z)));
}

#ifdef DOF_DISPLAY_IN
struct DofDisplayIn
{
    float4 color;   // rgb linear, a the back buffer's alpha
    float depth;    // the main depth's texel under the pixel's center
};

// A back buffer pixel decoded, and the main depth's texel under its center, nearest so no depth blends across an edge.
DofDisplayIn dof_display_in(int2 p)
{
    float2 back = float2(float(dof_counts.y), float(dof_counts.z));
    int2 main_size = int2(int(dof_display.x), int(dof_display.y));
    int x = min(int((float(p.x) + 0.5f) / back.x * dof_display.x), main_size.x - 1);
    int y = min(int((float(p.y) + 0.5f) / back.y * dof_display.y), main_size.y - 1);
    if (dof_display.z > 0.0f)
        y = main_size.y - 1 - y;
    float4 b = dof_load_back(p);
    DofDisplayIn o;
    float3 linear_color = dof_decode_display(float3(b.x, b.y, b.z));
    o.color = float4(linear_color.x, linear_color.y, linear_color.z, b.w);
    o.depth = dof_load_main_depth(int2(x, y));
    return o;
}
#endif

#ifdef DOF_DISPLAY_OUT
float4 dof_display_out(int2 p)
{
    float3 encoded = dof_encode_display(dof_load_blurred(p));
    return float4(encoded.x, encoded.y, encoded.z, dof_load_display(p).w);
}
#endif
