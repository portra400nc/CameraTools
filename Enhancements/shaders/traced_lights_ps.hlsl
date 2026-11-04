// The traced lights' pass, first at the bloom step: each of a traced shot's lights, which the game leaves out while the shot
// traces, added to the HDR scene's world pixels behind the visibility the trace kernel summed. Writes the whole target,
// because the result is copied over the game's HDR scene whole.
#include "gbuffer.hlsli"

cbuffer Trace : register(b1)
{
    float4 clip_rows[4];
    float4 eye_position;
    float4 sun_cone;
    float4 trace_params;
    uint4 trace_counts;
    uint4 trace_tiles;
    uint4 trace_render;     // width, height, flip, grid stride
};

cbuffer GatherLights : register(b3)
{
    float4 light_rows[16];
    uint4 light_info;
};

#include "local_light.hlsli"

Texture2D<float3> scene : register(t0);                 // mirror of the game's HDR scene
Texture2D<float4> albedo : register(t5);                // mirror of rt1, sRGB-encoded as stored
StructuredBuffer<float4> history : register(t6);        // the trace's: its .w counts the sun's samples, which the lights' follow
StructuredBuffer<float4> light_history : register(t7);  // each traced light's visibility summed

float3 srgb_to_linear(float3 c) { return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4); }

float3 main(float4 position : SV_Position) : SV_Target0
{
    int2 pixel = int2(position.xy);
    float3 color = scene.Load(int3(pixel, 0));
    float stored = depth.Load(int3(pixel, 0));
    if (stored <= 0 || !is_world(stencil.Load(int3(pixel, 0)).g))
        return color;
    uint at = uint(pixel.y) * trace_render.x + uint(pixel.x);
    float samples = history[at].w;
    if (samples <= 0)
        return color;
    // In the scene's frame, as trace_common.hlsli's scene_position.
    float v = (pixel.y + 0.5) / trace_render.y;
    if (trace_render.z != 0)
        v = 1 - v;
    float4 clip = float4((pixel.x + 0.5) / trace_render.x * 2 - 1, 1 - v * 2, stored, 1);
    float3 p = float3(dot(clip_rows[0], clip), dot(clip_rows[1], clip), dot(clip_rows[2], clip)) / dot(clip_rows[3], clip);
    float3 n = normalize(normals.Load(int3(pixel, 0)).xyz * 2 - 1);
    float4 seen = light_history[at] / samples;
    float3 light = 0;
    for (uint k = 0; k < light_info.x; k++)
        light += local_light_of(k, p, n) * seen[k];
    return color + srgb_to_linear(albedo.Load(int3(pixel, 0)).rgb) * light;
}
