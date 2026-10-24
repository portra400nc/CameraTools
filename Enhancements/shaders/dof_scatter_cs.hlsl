// The depth of field's highlights at half size: each texel of the prefilter that dof_detect finds a highlight takes the
// next slot of its field's sprites while one is free, and leaves the gather's source the rest of its light; every other
// texel goes to the source as it was (dof.hlsli).
#include "dof.cbuffer.hlsli"

Texture2D<float4> prefilter : register(t0);     // the prefilter's: rgb and the green CoC
Texture2D<float2> focus : register(t2);         // the autofocus's 1x1: the focus and the mean luminance
RWTexture2D<float4> source : register(u0);      // the gather's source, level 0
// Two DrawInstancedIndirect argument sets, the far field's then the near field's: 4 vertices, the sprites, 0, 0.
RWBuffer<uint> args : register(u1);

float4 dof_load_prefilter(int2 p) { return prefilter.Load(int3(clamp(p, int2(0, 0), int2(dof_sizes.xy) - 1), 0)); }
float2 dof_load_focus() { return focus.Load(int3(0, 0, 0)); }

#define DOF_SCATTER
#include "dof.hlsli"

// The far field's sprites, then the near field's from slot dof_highlight.w.
RWStructuredBuffer<DofSprite> sprites : register(u2);

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= dof_sizes.x || id.y >= dof_sizes.y)
        return;
    int2 h = int2(id.xy);
    DofDetect d = dof_detect(h);
    float4 kept = d.source;
    if (d.sprite)
    {
        uint field = d.as.at.z > 0.0f ? 0u : 1u;
        uint slot;
        InterlockedAdd(args[field * 4u + 1u], 1u, slot);
        if (slot < uint(dof_highlight.z))
        {
            sprites[field * uint(dof_highlight.w) + slot] = d.as;
            kept = d.rest;
        }
    }
    source[h] = kept;
}
