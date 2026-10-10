// Shared by every shader. Registers stay inside gpu.h's ranges: t0-t7, s0-s1, b0-b1.

cbuffer Constants : register(b0)
{
    float4x4 world_to_view;     // Unity's worldToCameraMatrix, column-major in memory as HLSL's default packing reads it
    float4x4 clip_to_view;      // inverse of GL.GetGPUProjectionMatrix(projection, false)
    float4 render_size;         // w, h, 1/w, 1/h
    float level;
    float ao_strength;
    float ao_radius;
    float flip;                 // 1: game targets are stored upside down, so their row 0 is the bottom of the image
    float projection_scale;     // pixels per world unit at distance 1: 0.5 * h * view_to_clip[1][1]
    uint decode;                // Decode in core/plan.h
    float foliage_ao_strength;  // multiplies ao_strength on stencil 129, 136 and 137
    float padding;
};

SamplerState point_clamp : register(s0);
SamplerState linear_clamp : register(s1);

// The game's character test: bit 0x04 marks characters, 0x80 the lit world. Sky is 0.
bool is_world(uint stencil) { return (stencil & 0x84u) == 0x80u; }

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
