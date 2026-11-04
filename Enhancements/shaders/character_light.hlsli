// What the character light pass's two draws share: the constants, the lights' reach (local_light.hlsli), the character's
// albedo and material id, and its normal, the face's mostly the rounded head's around head.xyz, as its stored normals all
// face forward.
#include "gbuffer.hlsli"

cbuffer CharacterLight : register(b1)
{
    float4 light_rows[16];  // core/trace.h's GatherLights rows in view space
    uint4 light_info;
    float4 head;            // view space; w 1 when CameraTools found the character's head
    float4 params;          // the game light's share, the rim's strength, 1 for soft shading, 0
};

#include "local_light.hlsli"

Texture2D<float4> albedo : register(t5);    // copy of rt1 from the combine step, sRGB-encoded as stored
Texture2D<float> material : register(t6);   // copy of rt3 from the combine step

static const float kHeadShare = 0.8;
static const uint kFaceMaterial = 22;
static const uint kEyeMaterial = 21;

bool is_character(uint stencil) { return (stencil & 0x04u) != 0; }

bool is_face(int2 pixel)
{
    uint id = uint(material.Load(int3(pixel, 0)) * 255 + 0.5) & 0x3Fu;
    return id == kFaceMaterial || id == kEyeMaterial;
}

float3 character_normal(int2 pixel, float3 p, bool face)
{
    float3 n = view_normal_at(pixel);
    return face && head.w > 0 ? normalize(kHeadShare * normalize(p - head.xyz) + (1 - kHeadShare) * n) : n;
}
