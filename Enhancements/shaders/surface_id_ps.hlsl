// Surface detail's debug view at strength 4: every G-buffer draw drawn again in a color of its pixel shader's hash, to
// find which shaders draw what. Reading SV_Position alone, it links to any vertex shader.
cbuffer SurfaceId : register(b13)
{
    float4 id_color;
};

float4 main(float4 position : SV_Position) : SV_Target0
{
    return id_color;
}
