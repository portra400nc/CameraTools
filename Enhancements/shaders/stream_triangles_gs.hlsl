// The scene capture's geometry shader for a tessellated draw: it takes each triangle the tessellator and the game's domain
// shader make and streams its three corners' SV_Position as three points, in order. DXVK's pass-through geometry shader,
// which a domain shader's own bytecode with stream output becomes, takes one point and emits one, so fed triangles it
// streamed only each triangle's first corner on the Deck (build 202: 1752 of 5256 corners of the terrain's draw 137, and
// exactly a third of every tessellated draw's on every face). DXVK reads a geometry shader's SV_Position input from the
// previous stage's built-in position, so this one shader serves every domain shader that writes one.
struct Corner
{
    float4 position : SV_Position;
};

[maxvertexcount(3)]
void main(triangle Corner corners[3], inout PointStream<Corner> stream)
{
    stream.Append(corners[0]);
    stream.Append(corners[1]);
    stream.Append(corners[2]);
}
