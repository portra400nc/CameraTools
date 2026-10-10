// The ambient pass's inputs, shared by its two draws, ao_ps.hlsl then ambient_ps.hlsl. Both bind t0 to t3 alike.
#include "common.hlsli"

Texture2D<float3> irradiance : register(t0);    // mirror of the game's diffuse irradiance (ambient pair rt1)
Texture2D<float4> normals : register(t1);       // mirror of G-buffer rt0, world normal * 0.5 + 0.5
Texture2D<float> depth : register(t2);          // mirror of the main depth, R32_FLOAT_X8X24_TYPELESS, reversed Z
Texture2D<uint2> stencil : register(t3);        // same mirror, X32_TYPELESS_G8X24_UINT; stencil in .g

float3 view_position_at(int2 pixel) { return view_position((pixel + 0.5) * render_size.zw, depth.Load(int3(pixel, 0))); }

float3 view_normal_at(int2 pixel) { return normalize(mul((float3x3)world_to_view, normals.Load(int3(pixel, 0)).xyz * 2 - 1)); }
