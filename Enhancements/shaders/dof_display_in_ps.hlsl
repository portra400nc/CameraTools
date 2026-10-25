// The depth of field on the display, first: the back buffer decoded to linear light, and the main depth at its size and
// upright, for the passes after (dof_display.hlsli).
#include "dof.cbuffer.hlsli"

Texture2D<float4> back : register(t0);      // mirror of the back buffer
Texture2D<float> main_depth : register(t1); // mirror of the main depth at the bloom step, reversed Z

float4 dof_load_back(int2 p) { return back.Load(int3(clamp(p, int2(0, 0), int2(dof_counts.yz) - 1), 0)); }
float dof_load_main_depth(int2 p) { return main_depth.Load(int3(clamp(p, int2(0, 0), int2(dof_display.xy) - 1), 0)); }

#define DOF_DISPLAY_IN
#include "dof_display.hlsli"

struct Targets
{
    float4 color : SV_Target0;
    float depth : SV_Target1;
};

Targets main(float4 position : SV_Position)
{
    DofDisplayIn d = dof_display_in(int2(position.xy));
    Targets o;
    o.color = d.color;
    o.depth = d.depth;
    return o;
}
