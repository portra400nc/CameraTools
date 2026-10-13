// The wetness pass: one draw writes the G-buffer's albedo, specular and smoothness, wet, into mirrors of them, which are
// copied back over the game's before anything reads them. A pixel it does not wet keeps its stored values exactly.
#include "gbuffer.hlsli"
#include "wet.hlsli"

Texture2D<float> smoothness : register(t0);     // mirror of G-buffer rt4
Texture2D<float4> albedo : register(t5);        // mirror of rt1, sRGB-encoded as stored; a is the material's scalar
Texture2D<float> material : register(t6);       // mirror of rt3: the material id / 255, with flags in bits 6 and 7
Texture2D<float4> specular : register(t7);      // mirror of rt2, the sRGB-encoded F0 as stored

static const float kWaterF0 = 0.02;

struct GBuffer
{
    float4 albedo : SV_Target0;
    float4 specular : SV_Target1;
    float smoothness : SV_Target2;
};

float3 srgb_to_linear(float3 c) { return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4); }
float3 linear_to_srgb(float3 c) { return c <= 0.0031308 ? c * 12.92 : 1.055 * pow(c, 1 / 2.4) - 0.055; }

// Ids whose rt2 or rt1.a hold something else, which the game's lighting draws test: 3, 6 and 23 keep a vector in rt2, and
// 13, 16, 19 and 24 swap rt1.a and the smoothness.
bool keeps_dry(uint id) { return id == 3 || id == 6 || id == 23 || id == 13 || id == 16 || id == 19 || id == 24; }

float hash(int2 cell)
{
    uint h = asuint(cell.x) * 73856093u ^ asuint(cell.y) * 19349663u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return float((h ^ (h >> 16)) & 0xFFFFu) / 65535.0;
}

float value_noise(float2 p)
{
    float2 cell = floor(p);
    float2 f = p - cell;
    f = f * f * (3 - 2 * f);
    int2 c = int2(cell);
    return lerp(lerp(hash(c), hash(c + int2(1, 0)), f.x), lerp(hash(c + int2(0, 1)), hash(c + int2(1, 1)), f.x), f.y);
}

// Blobs a few meters across on the ground, fixed in the world so they stay put as the camera moves.
float puddle_noise(float2 ground) { return 0.65 * value_noise(ground / 4) + 0.35 * value_noise(ground / 1.3 + 17); }

// The world position of a stored pixel. world_to_view is a rotation and a translation, so its inverse is the transposed
// rotation applied after taking the translation away.
float3 world_position_at(int2 pixel)
{
    float3 view = view_position_at(pixel);
    float3 translation = float3(world_to_view._m03, world_to_view._m13, world_to_view._m23);
    return mul(view - translation, (float3x3)world_to_view);
}

GBuffer main(float4 position : SV_Position)
{
    int2 pixel = int2(position.xy);
    GBuffer stored;
    stored.albedo = albedo.Load(int3(pixel, 0));
    stored.specular = specular.Load(int3(pixel, 0));
    stored.smoothness = smoothness.Load(int3(pixel, 0));
    uint s = stencil.Load(int3(pixel, 0)).g;
    uint id = uint(material.Load(int3(pixel, 0)) * 255 + 0.5) & ~0xC0u;
    if (!is_world(s) || keeps_dry(id))
        return stored;

    float up = normals.Load(int3(pixel, 0)).y * 2 - 1;
    float puddle = 0;
    if (up > 0.95)
        puddle = puddle_cover(puddle_noise(world_position_at(pixel).xz), up, wetness, puddles);
    float w = wet_share(wetness, up, puddle);

    GBuffer wet = stored;
    wet.albedo.rgb = linear_to_srgb(srgb_to_linear(stored.albedo.rgb) * (1 - wet_darkening * w));
    // The vegetation draw (stencil 136 and 137) reads rt2.x as a switch for its wrapped diffuse, so its F0 stays.
    if (s != 136 && s != 137)
        wet.specular.rgb = linear_to_srgb(lerp(srgb_to_linear(stored.specular.rgb), kWaterF0, w));
    wet.smoothness = wet_smoothness(stored.smoothness, w, puddle);
    return wet;
}
