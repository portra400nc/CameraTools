// Surface detail's mask for the game's terrain system (core/dxbc.h, world_position_layout 4): the world position in
// TEXCOORD1 to TEXCOORD3 .w, the rows of the tangent-to-world matrix. Otherwise as surface_mask.hlsli.
cbuffer SurfaceType : register(b13)
{
    float surface_type;
};

struct Terrain
{
    float4 position : SV_Position;
    float4 uv : TEXCOORD0;
    float4 row0 : TEXCOORD1;
    float4 row1 : TEXCOORD2;
    float4 row2 : TEXCOORD3;
};

struct Mask
{
    float4 world : SV_Target0;
    float2 depth : SV_Target1;
};

Mask main(Terrain t)
{
    Mask m;
    m.world = float4(t.row0.w, t.row1.w, t.row2.w, t.position.w);
    m.depth = float2(t.position.z, surface_type);
    return m;
}
