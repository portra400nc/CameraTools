// Leaf shading's constants: a copy of the game's vegetation lighting draw's cb0, changed so leaves shade by their facing.
// The draw lights a leaf by clamp((N.L w + x) w, 0.25, 0.75), w at cb0[240].w and x at cb0[241].x (0.6 and 0.9 in 7.2:
// 0.36 N.L + 0.54, nearly every leaf at the 0.75 ceiling), and adds a warm rim of cb0[240].xyz. At strength 1 the light
// becomes 0.5 N.L + 0.3, so only leaves facing the sun reach the ceiling and those turned away sink to the floor, and the
// rim is half as bright; between, the game's and that blend.
RWBuffer<uint> material : register(u0);     // the copy, a word an element

cbuffer LeafShading : register(b0)
{
    float4 leaf;    // x the strength, 0 to 1
};

static const uint kRim = 240 * 4, kScale = 240 * 4 + 3, kOffset = 241 * 4;
static const float kSlope = 0.5, kBase = 0.3;

[numthreads(1, 1, 1)]
void main()
{
    float w = asfloat(material[kScale]), x = asfloat(material[kOffset]), s = saturate(leaf.x);
    float slope = lerp(w * w, kSlope, s), base = lerp(x * w, kBase, s);
    float scale = sqrt(max(slope, 1e-4));
    material[kScale] = asuint(scale);
    material[kOffset] = asuint(base / scale);
    float rim = lerp(1, 0.5, s);
    [unroll] for (uint i = 0; i < 3; i++)
        material[kRim + i] = asuint(asfloat(material[kRim + i]) * rim);
}
