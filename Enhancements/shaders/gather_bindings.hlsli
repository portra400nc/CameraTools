// What the gather and reflection kernels share: their constants, buffers and the loads gather.hlsli asks its includer for.
// Registers stay inside gpu.h's ranges.
#ifndef GATHER_BINDINGS_HLSLI
#define GATHER_BINDINGS_HLSLI

cbuffer Trace : register(b0)
{
    float4 clip_rows[4];
    float4 eye_position;
    float4 sun_cone;
    float4 trace_params;
    uint4 trace_counts;
    uint4 trace_tiles;
    uint4 trace_render;
};

cbuffer Gather : register(b1)
{
    float4 gather_sun;          // rgb: the sun's color, what a white surface facing it shows
    float4 gather_shade;        // xyz: toward the sun as the game shades with it, unit length
    float4 gather_ambient[4];   // the ambient light at a hit of normal n: [0] + [1] n.x + [2] n.y + [3] n.z, rgb
    uint4 gather_counts;        // x: samples a pixel
};

cbuffer GatherLights : register(b3)
{
    float4 light_rows[16];      // core/trace.h's GatherLights: four rows a traced light
    uint4 light_info;           // x: the lights
};

cbuffer GatherFaces : register(b2)
{
    float4 face_rows[7][4];     // each face's scene_to_clip, by rows (core/light.h's FaceLight)
    float4 face_w[7];           // each face's depth_to_w
    uint4 face_info;            // x: the faces, y and z: their width and height
};

ByteAddressBuffer nodes : register(t0);         // core/bvh.h's BvhNode, 64 bytes each
ByteAddressBuffer triangles : register(t1);     // core/bvh.h's BvhTriangle, 48 bytes each, its light word in row 1
Texture2D<float4> normals : register(t2);       // mirror of G-buffer rt0, world normal * 0.5 + 0.5
Texture2D<float> depth : register(t3);          // mirror of the main depth, reversed Z
Texture2D<uint2> stencil : register(t4);        // same mirror; stencil in .g
ByteAddressBuffer tile_list : register(t5);     // core/trace.h's tile_order, x | y << 16
ByteAddressBuffer alpha : register(t6);         // core/alpha_bake.h's mask buffer: each alpha triangle's record, and the masks
ByteAddressBuffer sky : register(t7);           // core/light.h's sky map: kSkySide x kSkySide float4 texels, row by row
Texture2DArray<uint2> faces : register(t8);     // core/light.h's FaceLight pixels, a slice a face
RWStructuredBuffer<float4> history : register(u0);      // the trace's: occlusion sum, sun sum, their counts
RWStructuredBuffer<uint> counters : register(u1);       // core/trace.h's TraceCounter order, then its GatherCounter order
RWStructuredBuffer<float4> gather_hit : register(u2);   // per pixel, row by row: the hits' light summed, and the sun-lit hits
RWStructuredBuffer<float4> gather_sky : register(u3);   // per pixel: the misses' sky summed, and the misses

groupshared uint stack_lds[64 * 24];

uint4 node_row(uint node, uint row) { return nodes.Load4(node * 64u + row * 16u); }
uint4 triangle_row(uint index, uint row) { return triangles.Load4(index * 48u + row * 16u); }
float load_depth(int2 pixel) { return depth.Load(int3(pixel, 0)); }
float3 load_normal(int2 pixel) { return normals.Load(int3(pixel, 0)).xyz * 2.0f - 1.0f; }
uint load_stencil(int2 pixel) { return stencil.Load(int3(pixel, 0)).g; }
uint alpha_word(uint index) { return alpha.Load(index * 4u); }
uint4 alpha_row(uint index) { return alpha.Load4(index * 4u); }
uint4 sky_row(uint index) { return sky.Load4(index * 16u); }
uint2 face_texel(uint face, uint2 pixel) { return faces.Load(uint4(pixel.x, pixel.y, face, 0u)); }

#define LOOP [loop]
#define XYZ(v) ((v).xyz)
#define STACK_STORE(lane, slot, value) stack_lds[(lane) * 24u + (slot)] = (value)
#define STACK_LOAD(lane, slot) stack_lds[(lane) * 24u + (slot)]
#include "gather.hlsli"

#endif
