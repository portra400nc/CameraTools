// The depth of field's highlight sprites: instance i is sprite i of one field's list, its quad a strip of 4 vertices around
// its center in half-size pixels. Instances past the frame's cap, which dof_scatter_cs.hlsl counted but did not write,
// collapse to nothing.
#include "dof.cbuffer.hlsli"
#include "dof.hlsli"

StructuredBuffer<DofSprite> sprites : register(t0);

struct Corner
{
    float4 position : SV_Position;
    nointerpolation float4 at : TEXCOORD0;
    nointerpolation float4 light : TEXCOORD1;
};

Corner main(uint vertex : SV_VertexID, uint instance : SV_InstanceID)
{
    Corner o;
    DofSprite s = sprites[instance];
    o.at = s.at;
    o.light = s.light;
    float2 at = float2(s.at.x + ((vertex & 1u) != 0u ? s.at.w : -s.at.w), s.at.y + ((vertex & 2u) != 0u ? s.at.w : -s.at.w));
    o.position = instance < uint(dof_highlight.z) ? float4(at.x / float(dof_sizes.x) * 2.0f - 1.0f, 1.0f - at.y / float(dof_sizes.y) * 2.0f, 0.0f, 1.0f)
                                                   : float4(-2.0f, -2.0f, 0.0f, 1.0f);
    return o;
}
