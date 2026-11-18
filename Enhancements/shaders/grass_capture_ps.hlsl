// The grass pass's copy at the combine step, where the stencil still holds the scene: 1 on a grass blade (stencil 129, or
// tall grass, material id 3 under 137), 0.5 on ground (stencil 128 or 144), 0 on anything else. By the bloom step the
// stencil holds other things.
#include "gbuffer.hlsli"

Texture2D<float> material : register(t6);   // G-buffer rt3

static const uint kTallGrassMaterial = 3;

float main(float4 position : SV_Position) : SV_Target0
{
    int3 p = int3(position.xy, 0);
    uint s = stencil.Load(p).g;
    uint id = uint(material.Load(p) * 255 + 0.5) & 0x3Fu;
    return s == 129 || (s == 137 && id == kTallGrassMaterial) ? 1 : s == 128 || s == 144 ? 0.5 : 0;
}
