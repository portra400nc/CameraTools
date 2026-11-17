// The traced foliage pass at the bloom step: grass and leaves in the HDR scene times the shade the trace's apply draw
// wrote for them at the combine step, where the stencil still marked them. Writes the whole target, because the result is
// copied over the game's HDR scene whole.
#include "common.hlsli"

Texture2D<float3> scene : register(t0);     // mirror of the HDR scene
Texture2D<float> shade : register(t4);      // the apply draw's foliage shade: below 1 on foliage, 1 elsewhere

float4 main(float4 position : SV_Position) : SV_Target0
{
    int2 pixel = int2(position.xy);
    return float4(scene.Load(int3(pixel, 0)) * shade.Load(int3(pixel, 0)), 1);
}
