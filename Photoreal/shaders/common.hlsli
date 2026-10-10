// Shared by every shader. Registers stay inside gpu.h's ranges: t0-t7, s0-s2, b0-b4.

cbuffer Constants : register(b0)
{
    float4x4 world_to_view;     // Unity's worldToCameraMatrix, column-major in memory as HLSL's default packing reads it
    float4x4 clip_to_view;      // inverse of GL.GetGPUProjectionMatrix(projection, false)
    float4x4 view_to_clip;      // GL.GetGPUProjectionMatrix(projection, false)
    float4 render_size;         // w, h, 1/w, 1/h; the back buffer's in the debug composite
    float level;
    float ao_strength;
    float ao_radius;
    float flip;                 // 1: game targets are stored upside down, so their row 0 is the bottom of the image
    float projection_scale;     // pixels per world unit at distance 1: 0.5 * h * view_to_clip[1][1]
    uint decode;                // Decode in core/plan.h
    float foliage_ao_strength;  // multiplies ao_strength on stencil 129, 136 and 137
    float contact_length;       // meters
    float3 toward_sun;          // world space, unit length, or zero when there is no sun
    float contact_strength;
    float contact_thickness;    // meters
    uint label_length;          // glyphs in label
    float contact_foliage_strength;  // multiplies contact_strength on stencil 129, 136 and 137
    float padding;
    uint4 label[2];             // the debug composite's view name, glyph indices from encode_label, four per uint, low byte first
    float3 sun_color;           // linear RGB times intensity
    float atmosphere_density;   // extinction per meter at the camera's height
    float3 sky_color;           // linear RGB
    float height_falloff;       // per meter
    float sun_scatter;
    float anisotropy;           // Henyey-Greenstein g
    float exposure_scale;       // 2 ^ tonemap.exposure_ev
    uint curve;                 // Curve in core/plan.h
    float bloom_strength;       // multiplies the game's bloom intensity
    float saturation;
    float contrast;
    float padding2;
    uint sun_cascades;          // the cascades the game uses, from SunShadowConstants in core/sun.h
    uint sun_columns;           // the atlas's grid of cascade tiles
    uint sun_rows;
    float sun_light_size;       // penumbra width per meter between caster and receiver
    float sun_min_penumbra;     // meters
    float sun_strength;
    float2 padding3;
};

SamplerState point_clamp : register(s0);
SamplerState linear_clamp : register(s1);

// The game's character test: bit 0x04 marks characters, 0x80 the lit world. Sky is 0.
bool is_world(uint stencil) { return (stencil & 0x84u) == 0x80u; }

bool is_grass(uint stencil) { return stencil == 129; }

// Grass, vegetation and foliage: alpha-tested, so their depth is noisy at the pixel scale.
bool is_foliage(uint stencil) { return stencil == 129 || stencil == 136 || stencil == 137; }

// The texture coordinate in a stored game target of a point on screen, uv (0, 0) being the top left.
float2 stored_uv(float2 uv) { return flip > 0.5 ? float2(uv.x, 1 - uv.y) : uv; }

// Unity's view space position of a stored pixel, from reversed Z. Reads flip like the debug views, so an upright view
// means this reconstruction is upright too.
float3 view_position(float2 stored, float reversed_depth)
{
    float2 screen = stored_uv(stored);
    float4 clip = float4(screen.x * 2 - 1, 1 - screen.y * 2, reversed_depth, 1);
    float4 view = mul(clip_to_view, clip);
    return view.xyz / view.w;
}

// The stored texture coordinate of a view space point in front of the camera; view_position's inverse.
float2 stored_of(float3 view)
{
    float4 clip = mul(view_to_clip, float4(view, 1));
    float2 ndc = clip.xy / clip.w;
    return stored_uv(float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5));
}
