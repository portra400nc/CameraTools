// Surface detail's mask for the near terrain (core/dxbc.h, world_position_layout 3): its vertex shader writes no world
// position but the vector from the point to the camera in TEXCOORD3.xyz, and the camera's position this frame is row 5
// of its cb1, which the mask draw binds here at b12. Otherwise as surface_mask.hlsli.
cbuffer GameCamera : register(b12)
{
    float4 game_camera[7];
};

cbuffer SurfaceType : register(b13)
{
    float surface_type;
};

struct Ground
{
    float4 position : SV_Position;
    float4 color : COLOR0;
    float4 t0 : TEXCOORD0;
    float4 t1 : TEXCOORD1;
    float4 t2 : TEXCOORD2;
    float3 to_camera : TEXCOORD3;
};

struct Mask
{
    float4 world : SV_Target0;
    float2 depth : SV_Target1;
};

Mask main(Ground g)
{
    Mask m;
    m.world = float4(game_camera[5].xyz - g.to_camera, g.position.w);
    m.depth = float2(g.position.z, surface_type);
    return m;
}
