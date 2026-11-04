// The scene capture's geometry shader for a tessellated draw with an alpha test, such as the terrain's holes: it does what
// stream_triangles_gs.hlsl does and also passes registers 1 to 7 of the game's domain shader through, as ATTR1 to ATTR7, for
// stream output to pick the alpha program's inputs from. DXVK links stages by register, whatever the semantics, and reads
// the position from the built-in. The registers the domain shader does not write are never streamed.
struct Corner
{
    float4 position : SV_Position;
    float4 a1 : ATTR1;
    float4 a2 : ATTR2;
    float4 a3 : ATTR3;
    float4 a4 : ATTR4;
    float4 a5 : ATTR5;
    float4 a6 : ATTR6;
    float4 a7 : ATTR7;
};

[maxvertexcount(3)]
void main(triangle Corner corners[3], inout PointStream<Corner> stream)
{
    stream.Append(corners[0]);
    stream.Append(corners[1]);
    stream.Append(corners[2]);
}
