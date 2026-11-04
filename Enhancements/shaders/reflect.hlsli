// A traced shot's reflections: one ray a sample from the GGX lobe of a world pixel, lit as the gather's rays are, whose mean
// replaces the probes the game's combine pass reads. The includer defines what gather.hlsli asks for, and
//   float load_smoothness(int2 pixel), uint load_material(int2 pixel)  G-buffer rt4, and rt3's id without its top two bits
//   float4 reflect_params   x: what scales roughness, y: how much brighter light sources get, w unused
#ifndef REFLECT_HLSLI
#define REFLECT_HLSLI
#include "reflect_common.hlsli"

static const uint kReflectSalt = 0x9B05688Cu;

// Materials whose G-buffer holds something else than F0 and smoothness (shaders/wetness_ps.hlsl): they keep the game's.
bool reflects_of(uint material)
{
    return !(material == 3u || material == 6u || material == 23u || material == 13u || material == 16u || material == 19u || material == 24u);
}

// Sample index of a pixel's lobe, mirrored above the surface where it leans past it.
GatherData reflect_pixel_of(int2 pixel, uint index, uint lane)
{
    GatherData r;
    r.light = float4(0.0f, 0.0f, 0.0f, 0.0f);
    r.occlusion = 1.0f;
    r.world = 0u;
    r.guard = 0u;
    r.overflow = 0u;
    r.rays = 0u;
    r.lit = 0u;
    r.source = 0u;
    uint s = load_stencil(pixel);
    float depth = load_depth(pixel);
    if (!is_world_stencil(s) || !(depth > 0.0f) || !reflects_of(load_material(pixel)))
        return r;
    r.world = 1u;
    float3 eye = XYZ(eye_position);
    float3 p = scene_position(pixel, depth);
    float dist = length(p - eye);
    float3 view = (eye - p) / dist;
    float3 stored = load_normal(pixel);
    float stored_length = length(stored);
    float3 ng = geometric_normal(pixel, p, eye, stored_length > 1.0e-6f ? stored / stored_length : view);
    float3 n = stored_length > 1.0e-6f ? stored / stored_length : ng;
    uint seed = pixel_seed_of(pixel, trace_tiles.w);
    float rough = reflect_roughness_of(load_smoothness(pixel), reflect_params.x);
    float a = max(rough * rough, 1.0e-3f);
    float2 u = sequence_point_of(seed, kReflectSalt, index);
    float c = sqrt((1.0f - u.x) / (1.0f + (a * a - 1.0f) * u.x));
    float sine = sqrt(max(0.0f, 1.0f - c * c));
    float phi = 6.28318531f * u.y;
    Basis b = basis_of(n);
    float3 m = b.t * (sine * cos(phi)) + b.b * (sine * sin(phi)) + n * c;
    float3 d = m * (2.0f * dot(view, m)) - view;
    float below = dot(d, ng);
    if (below < 0.0f)
        d = d - ng * (2.0f * below);
    return gather_ray_of(r, p + ng * (0.02f + 1.0e-4f * dist), normalize(d), eye, seed, index, float2(reflect_params.y, gather_sun.w), 1u, 1.0f, lane);
}

#endif
