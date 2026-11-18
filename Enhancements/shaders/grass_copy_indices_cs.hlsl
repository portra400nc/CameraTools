// The indices of a grass draw's clump, copies times over, into our copied mesh (grass_copy_vertices_cs.hlsl), and the
// draw's indirect arguments for them: its own index count times the copies, from our first index, its instances as they
// are. The game's indices are 16-bit; ours are 32.
Buffer<uint> source : register(t0);     // the game's index buffer, two indices a word
Buffer<uint> game : register(t1);       // the draw's arguments: index count, instances, first index, base vertex, first instance
RWBuffer<uint> indices : register(u0);
RWBuffer<uint> args : register(u1);

cbuffer GrassCopy : register(b0)
{
    uint4 count;    // x the vertices a copy, y the copies
};

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    uint each = game[0], first = game[2], n = count.y, i = id.x;
    if (i == 0)
    {
        args[0] = each * n;
        args[1] = game[1];
        args[2] = 0;
        args[3] = game[3];
        args[4] = game[4];
    }
    if (i >= each * n)
        return;
    uint at = first + i % each;
    uint index = (source[at >> 1] >> ((at & 1) * 16)) & 0xFFFF;
    indices[i] = index + (i / each) * count.x;
}
