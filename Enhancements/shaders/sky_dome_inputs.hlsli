// The game's sky dome vertex shader's outputs up to TEXCOORD8, in its registers, as D3D11 links the stages (core/dxbc.h,
// sky_direction_layout): TEXCOORD8.xyz is the direction from this frame's camera to the vertex, normalized.
struct Dome
{
    float4 position : SV_Position;
    float4 t0 : TEXCOORD0;
    float4 t1 : TEXCOORD1;
    float4 t2 : TEXCOORD2;
    float4 t3 : TEXCOORD3;
    float4 t4 : TEXCOORD4;
    float2 t5 : TEXCOORD5;
    float4 t6 : TEXCOORD6;
    float4 t7 : TEXCOORD7;
    float4 toward : TEXCOORD8;
};
