// The lens flare's light alone, added to a DNG's linear light with additive blending, where flare_ps.hlsl screens it over the
// picture: the DNG keeps the flare's energy past white for the raw editor.
#include "flare.cbuffer.hlsli"

Texture2D<float> visibility : register(t1);     // flare_visibility_ps.hlsl's texel

float flare_load_visibility() { return visibility.Load(int3(0, 0, 0)); }

#include "flare.hlsli"

float4 main(float4 position : SV_Position) : SV_Target0
{
    return float4(flare_light(position.xy), 0);
}
