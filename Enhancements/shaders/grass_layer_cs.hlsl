// A grass layer's constants: copies of a game grass draw's vertex shader constant buffers, changed so the draw places
// its blades again a little off, turned and smaller. The game's grass vertex shaders take each blade's place from their
// instance buffer less the grass origin at cb1[10], its turn from cb0[47].z plus a share of cb0[47].w, and its size from
// cb0[48].x plus a share of cb0[48].y; moving the origin by -shift moves every blade by shift.
RWBuffer<uint> material : register(u0);         // copy of the draw's cb0, from its bound range's start, a word each
RWBuffer<uint> frame : register(u1);            // copy of its cb1

cbuffer GrassLayer : register(b0)
{
    float4 layer;   // x, z: meters the blades move along world x and z; y: radians they turn; w: their size's scale
};

static const uint kOrigin = 10 * 4, kTurn = 47 * 4 + 2, kSize = 48 * 4;     // in words

[numthreads(1, 1, 1)]
void main()
{
    frame[kOrigin] = asuint(asfloat(frame[kOrigin]) - layer.x);
    frame[kOrigin + 2] = asuint(asfloat(frame[kOrigin + 2]) - layer.z);
    material[kTurn] = asuint(asfloat(material[kTurn]) + layer.y);
    material[kSize] = asuint(asfloat(material[kSize]) * layer.w);
    material[kSize + 1] = asuint(asfloat(material[kSize + 1]) * layer.w);
}
