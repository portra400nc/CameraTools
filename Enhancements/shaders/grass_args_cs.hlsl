// The grass port's indirect arguments, a copy of the game's grass draw's: its instance count times the copies, and for
// the port's own clump, its index count from the start.
RWBuffer<uint> args : register(u0);     // index count, instances, first index, base vertex, first instance

cbuffer GrassArgs : register(b0)
{
    uint4 fix;      // x copies; y the clump's index count, 0 to keep the game's mesh
};

[numthreads(1, 1, 1)]
void main()
{
    args[1] = args[1] * fix.x;
    if (fix.y != 0)
    {
        args[0] = fix.y;
        args[2] = 0;
        args[3] = 0;
    }
}
