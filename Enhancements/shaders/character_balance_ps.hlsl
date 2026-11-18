// The character light pass's balance meter, after its light draw: each pixel of a 64x32 target takes 8x8 points of its
// share of the lit picture and, where its share holds both character and world points, writes how brightly each is lit:
// the sums of log2 of their luminance over their albedo's, and their counts. Shares without both write zeros, so only
// the world right around the characters counts. The target's mips then average it.
#include "character_light.hlsli"

Texture2D<float3> scene : register(t0);     // the light draw's result

static const float kDarkestAlbedo = 0.04;   // points darker than this say little about their light

float3 srgb_to_linear(float3 c) { return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4); }
float luminance_of(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }

float4 main(float4 position : SV_Position) : SV_Target0
{
    float2 block = render_size.xy / float2(64, 32);
    float2 corner = floor(position.xy) * block;
    float4 sums = 0;
    [loop] for (int y = 0; y < 8; y++)
    {
        [loop] for (int x = 0; x < 8; x++)
        {
            int3 p = int3(corner + (float2(x, y) + 0.5) * block / 8, 0);
            if (depth.Load(p) <= 0)
                continue;
            uint s = stencil.Load(p).g;
            bool character = is_character(s);
            if (!character && (s & 0x84u) != 0x80u)
                continue;
            float albedo_light = luminance_of(srgb_to_linear(albedo.Load(p).rgb));
            if (albedo_light < kDarkestAlbedo)
                continue;
            float light = log2(max(luminance_of(scene.Load(p)), 1e-5) / albedo_light);
            sums += character ? float4(0, 0, light, 1) : float4(light, 1, 0, 0);
        }
    }
    return sums.y > 0 && sums.w > 0 ? sums / 64 : 0;
}
