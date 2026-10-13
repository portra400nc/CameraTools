// One triangle that covers the target. Draw(3, 0) with no input layout.
struct Varyings
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

Varyings main(uint id : SV_VertexID)
{
    Varyings o;
    o.uv = float2((id << 1) & 2, id & 2);
    o.position = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}
