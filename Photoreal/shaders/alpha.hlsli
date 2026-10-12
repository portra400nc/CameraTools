// The alpha test of the traced scene's alpha-tested triangles: core/alpha.h's programs, interpreted at a hit with the
// game's own discard logic, its constants as the draw had them and its texture's alpha channel from the atlas. Shared by
// shaders/bvh.hlsli and, compiled as C++ through core/hlsl.h, by core/kernel.cpp; it keeps to what core/hlsl.h supports.
//
// The includer defines:
//   uint alpha_word(uint index)            the alpha buffer's word (core/alpha.h's layout)
//   uint4 alpha_row(uint index)            its four words from index
//   ALPHA_LOAD(lane, r), ALPHA_STORE(lane, r, value)  the lane's register r of kAlphaRegisters, uint4 each

// 8 temporaries, then 3 of interpolated inputs, then the current instruction's 3 sources, which pass through registers so
// the shader holds one copy of the source fetch and of the arithmetic.
static const uint kAlphaRegisters = 14u;
static const uint kAlphaInputs = 8u;
static const uint kAlphaSources = 11u;
static const uint kOpaqueRecord = 0xFFFFFFu;

// core/alpha.h's alpha_op numbers.
static const uint kOpMov = 0u, kOpMovc = 1u, kOpAdd = 2u, kOpMul = 3u, kOpMad = 4u, kOpDiv = 5u, kOpMin = 6u, kOpMax = 7u, kOpDp2 = 8u,
    kOpDp3 = 9u, kOpDp4 = 10u, kOpLt = 11u, kOpGe = 12u, kOpEq = 13u, kOpNe = 14u, kOpFtoi = 15u, kOpFtou = 16u, kOpRoundNe = 17u,
    kOpRoundNi = 18u, kOpRoundPi = 19u, kOpRoundZ = 20u, kOpFrc = 21u, kOpRsq = 22u, kOpSqrt = 23u, kOpExp = 24u, kOpLog = 25u, kOpRcp = 26u,
    kOpSample = 27u, kOpAnd = 28u, kOpOr = 29u, kOpXor = 30u, kOpNot = 31u, kOpIadd = 32u, kOpIeq = 33u, kOpIne = 34u, kOpIlt = 35u,
    kOpIge = 36u, kOpUlt = 37u, kOpUge = 38u, kOpImin = 39u, kOpImax = 40u, kOpUmin = 41u, kOpUmax = 42u, kOpIneg = 43u, kOpIshl = 44u,
    kOpIshr = 45u, kOpUshr = 46u, kOpItof = 47u, kOpUtof = 48u, kOpIfNz = 60u, kOpIfZ = 61u, kOpElse = 62u, kOpDiscardNz = 63u,
    kOpDiscardZ = 64u;

uint component_of(uint4 v, uint c) { return c == 0u ? v.x : c == 1u ? v.y : c == 2u ? v.z : v.w; }

uint4 with_component(uint4 v, uint c, uint value)
{
    return uint4(c == 0u ? value : v.x, c == 1u ? value : v.y, c == 2u ? value : v.z, c == 3u ? value : v.w);
}

uint byte_of(uint4 v, uint k) { return (component_of(v, k / 4u) >> ((k % 4u) * 8u)) & 255u; }

// D3D's float to integer conversions: NaN gives 0, and values past the range clamp to it.
uint ftoi_of(float f)
{
    if (!(f == f))
        return 0u;
    if (f >= 2147483647.0f)
        return 0x7FFFFFFFu;
    if (f <= -2147483648.0f)
        return 0x80000000u;
    return asuint(int(f));
}

uint ftou_of(float f)
{
    if (!(f > 0.0f))
        return 0u;
    if (f >= 4294967295.0f)
        return 0xFFFFFFFFu;
    return uint(f);
}

uint bool_bits(bool b) { return b ? 0xFFFFFFFFu : 0u; }

// One component of an element-wise operation, on bits.
uint alpha_alu(uint op, uint a, uint b, uint c)
{
    float fa = asfloat(a);
    float fb = asfloat(b);
    float fc = asfloat(c);
    if (op == kOpMov) return a;
    if (op == kOpMovc) return a != 0u ? b : c;
    if (op == kOpAdd) return asuint(fa + fb);
    if (op == kOpMul) return asuint(fa * fb);
    if (op == kOpMad) return asuint(fa * fb + fc);
    if (op == kOpDiv) return asuint(fa / fb);
    if (op == kOpMin) return asuint(min(fa, fb));
    if (op == kOpMax) return asuint(max(fa, fb));
    if (op == kOpLt) return bool_bits(fa < fb);
    if (op == kOpGe) return bool_bits(fa >= fb);
    if (op == kOpEq) return bool_bits(fa == fb);
    if (op == kOpNe) return bool_bits(!(fa == fb));
    if (op == kOpFtoi) return ftoi_of(fa);
    if (op == kOpFtou) return ftou_of(fa);
    if (op == kOpRoundNe) return asuint(round(fa));
    if (op == kOpRoundNi) return asuint(floor(fa));
    if (op == kOpRoundPi) return asuint(ceil(fa));
    if (op == kOpRoundZ) return asuint(trunc(fa));
    if (op == kOpFrc) return asuint(fa - floor(fa));
    if (op == kOpRsq) return asuint(rsqrt(fa));
    if (op == kOpSqrt) return asuint(sqrt(fa));
    if (op == kOpExp) return asuint(exp2(fa));
    if (op == kOpLog) return asuint(log2(fa));
    if (op == kOpRcp) return asuint(1.0f / fa);
    if (op == kOpAnd) return a & b;
    if (op == kOpOr) return a | b;
    if (op == kOpXor) return a ^ b;
    if (op == kOpNot) return ~a;
    if (op == kOpIadd) return a + b;
    if (op == kOpIeq) return bool_bits(a == b);
    if (op == kOpIne) return bool_bits(a != b);
    if (op == kOpIlt) return bool_bits(asint(a) < asint(b));
    if (op == kOpIge) return bool_bits(asint(a) >= asint(b));
    if (op == kOpUlt) return bool_bits(a < b);
    if (op == kOpUge) return bool_bits(a >= b);
    if (op == kOpImin) return asint(a) < asint(b) ? a : b;
    if (op == kOpImax) return asint(a) > asint(b) ? a : b;
    if (op == kOpUmin) return a < b ? a : b;
    if (op == kOpUmax) return a > b ? a : b;
    if (op == kOpIneg) return 0u - a;
    if (op == kOpIshl) return a << (b & 31u);
    if (op == kOpIshr) return asuint(asint(a) >> int(b & 31u));
    if (op == kOpUshr) return a >> (b & 31u);
    if (op == kOpItof) return asuint(float(asint(a)));
    if (op == kOpUtof) return asuint(float(a));
    return 0u;
}

// A texel coordinate of an atlas tile under a sampler's address mode (D3D11_TEXTURE_ADDRESS_MODE: 1 wrap, 2 mirror, 3
// clamp, 4 border, 5 mirror once), or -1 outside a border.
int alpha_address(int i, uint mode)
{
    if (mode == 1u)
        return i & 255;
    if (mode == 2u)
    {
        int m = i & 511;
        return m < 256 ? m : 511 - m;
    }
    if (mode == 4u)
        return i < 0 || i > 255 ? -1 : i;
    if (mode == 5u)
        return min(i < 0 ? -1 - i : i, 255);
    return min(max(i, 0), 255);
}

float alpha_texel(uint atlas, uint tile, int x, int y)
{
    if (x < 0 || y < 0)
        return 0.0f;
    uint at = tile * 65536u + uint(y) * 256u + uint(x);
    return float((alpha_word(atlas + at / 4u) >> ((at % 4u) * 8u)) & 255u) / 255.0f;
}

// The tile bilinear at (u, v), with the sampler's address modes in bits 0 to 2 and 3 to 5.
float alpha_sample(uint atlas, uint tile, uint address, float u, float v)
{
    float x = min(max(u, -65536.0f), 65536.0f) * 256.0f - 0.5f;
    float y = min(max(v, -65536.0f), 65536.0f) * 256.0f - 0.5f;
    float x0 = floor(x);
    float y0 = floor(y);
    float fx = x - x0;
    float fy = y - y0;
    int ix = int(x0);
    int iy = int(y0);
    uint mu = address & 7u;
    uint mv = (address >> 3) & 7u;
    int xa = alpha_address(ix, mu);
    int xb = alpha_address(ix + 1, mu);
    int ya = alpha_address(iy, mv);
    int yb = alpha_address(iy + 1, mv);
    float top = lerp(alpha_texel(atlas, tile, xa, ya), alpha_texel(atlas, tile, xb, ya), fx);
    float bottom = lerp(alpha_texel(atlas, tile, xa, yb), alpha_texel(atlas, tile, xb, yb), fx);
    return lerp(top, bottom, fy);
}

// A source operand's four components, swizzled, its modifiers applied as floats or, for an operation on integers, as
// integers. block is the draw's block, literals the program's literals.
uint4 alpha_source(uint s, bool integer, uint block, uint rows, uint literals, uint lane)
{
    uint kind = s & 3u;
    uint index = s >> 18;
    uint4 v = uint4(0u, 0u, 0u, 0u);
    if (kind == 0u)
        v = ALPHA_LOAD(lane, min(index, kAlphaRegisters - 1u));
    else if (kind == 3u)
        v = alpha_row(literals + index * 4u);
    else
    {
        int row = int(index);
        if (kind == 2u)
            row += asint(component_of(ALPHA_LOAD(lane, min((s >> 12) & 15u, kAlphaRegisters - 1u)), (s >> 16) & 3u));
        if (row >= 0 && uint(row) < rows)
            v = alpha_row(block + 4u + uint(row) * 4u);
    }
    uint4 o = uint4(component_of(v, (s >> 2) & 3u), component_of(v, (s >> 4) & 3u), component_of(v, (s >> 6) & 3u), component_of(v, (s >> 8) & 3u));
    if (((s >> 10) & 3u) == 0u)
        return o;
    bool negate = ((s >> 10) & 1u) != 0u;
    bool absolute = ((s >> 11) & 1u) != 0u;
    if (integer)
    {
        if (absolute)
            o = uint4(asint(o.x) < 0 ? 0u - o.x : o.x, asint(o.y) < 0 ? 0u - o.y : o.y, asint(o.z) < 0 ? 0u - o.z : o.z, asint(o.w) < 0 ? 0u - o.w : o.w);
        if (negate)
            o = uint4(0u - o.x, 0u - o.y, 0u - o.z, 0u - o.w);
        return o;
    }
    // Sign bit operations, as the hardware applies float modifiers, so they also move a NaN's sign.
    if (absolute)
        o = uint4(o.x & 0x7FFFFFFFu, o.y & 0x7FFFFFFFu, o.z & 0x7FFFFFFFu, o.w & 0x7FFFFFFFu);
    if (negate)
        o = uint4(o.x ^ 0x80000000u, o.y ^ 0x80000000u, o.z ^ 0x80000000u, o.w ^ 0x80000000u);
    return o;
}

// Whether the alpha test keeps a hit at barycentric (u, v) of the triangle whose record starts at word record of the
// records: false when a kept discard of its draw's pixel shader fires there.
bool alpha_keeps(uint record, float u, float v, uint lane)
{
    uint4 header = alpha_row(0u);
    uint at = header.x + record;
    uint block = header.z + alpha_word(at);
    uint4 draw = alpha_row(block);
    uint program = header.y + draw.x;
    uint4 p0 = alpha_row(program);
    uint4 map = alpha_row(program + 2u);
    uint count = p0.x & 0xFFFFu;
    uint floats = p0.y & 0xFFFFu;
    uint flat = p0.y >> 16;
    uint literals = program + 8u + count * 4u;
    float w = 1.0f - u - v;

    // The inputs, each from the three corners' streamed floats: barycentric, which equals the rasterizer's perspective-correct
    // interpolation, or the first corner's bits for a flat one.
    LOOP for (uint r = 0u; r < 3u; r++)
    {
        uint4 value = uint4(0u, 0u, 0u, 0u);
        LOOP for (uint c = 0u; c < 4u; c++)
        {
            uint a = byte_of(map, r * 4u + c);
            if (a == 255u)
                continue;
            uint c0 = alpha_word(at + 1u + a);
            uint c1 = alpha_word(at + 1u + floats + a);
            uint c2 = alpha_word(at + 1u + 2u * floats + a);
            uint bits = ((flat >> a) & 1u) != 0u ? c0 : asuint(asfloat(c0) * w + asfloat(c1) * u + asfloat(c2) * v);
            value = with_component(value, c, bits);
        }
        ALPHA_STORE(lane, kAlphaInputs + r, value);
    }
    LOOP for (uint t = 0u; t < kAlphaInputs; t++)
        ALPHA_STORE(lane, t, uint4(0u, 0u, 0u, 0u));

    LOOP for (uint pc = 0u; pc < count; pc++)
    {
        uint4 ins = alpha_row(program + 8u + pc * 4u);
        uint op = ins.x & 255u;
        uint target = ins.x >> 20;
        // Jumps only go forward, so the program ends.
        if (op == kOpElse)
        {
            pc = max(target, pc + 1u) - 1u;
            continue;
        }
        bool integer = op >= kOpAnd && op <= kOpUtof;
        LOOP for (uint k = 0u; k < 3u; k++)
            ALPHA_STORE(lane, kAlphaSources + k, alpha_source(k == 0u ? ins.y : k == 1u ? ins.z : ins.w, integer, block, draw.w, literals, lane));
        uint4 a = ALPHA_LOAD(lane, kAlphaSources);
        if (op == kOpIfNz || op == kOpIfZ)
        {
            if ((a.x != 0u) != (op == kOpIfNz))
                pc = max(target, pc + 1u) - 1u;
            continue;
        }
        if (op == kOpDiscardNz || op == kOpDiscardZ)
        {
            if ((a.x != 0u) == (op == kOpDiscardNz))
                return false;
            continue;
        }
        uint4 result = uint4(0u, 0u, 0u, 0u);
        if (op == kOpSample)
        {
            uint value = asuint(alpha_sample(header.w, draw.y, draw.z, asfloat(a.x), asfloat(a.y)));
            result = uint4(value, value, value, value);
        }
        else
        {
            uint4 b = ALPHA_LOAD(lane, kAlphaSources + 1u);
            if (op == kOpDp2 || op == kOpDp3 || op == kOpDp4)
            {
                float d = asfloat(a.x) * asfloat(b.x) + asfloat(a.y) * asfloat(b.y);
                if (op != kOpDp2)
                    d += asfloat(a.z) * asfloat(b.z);
                if (op == kOpDp4)
                    d += asfloat(a.w) * asfloat(b.w);
                result = uint4(asuint(d), asuint(d), asuint(d), asuint(d));
            }
            else
            {
                uint4 c = ALPHA_LOAD(lane, kAlphaSources + 2u);
                LOOP for (uint k = 0u; k < 4u; k++)
                    result = with_component(result, k, alpha_alu(op, component_of(a, k), component_of(b, k), component_of(c, k)));
            }
        }
        if (((ins.x >> 16) & 1u) != 0u)
            result = uint4(asuint(saturate(asfloat(result.x))), asuint(saturate(asfloat(result.y))), asuint(saturate(asfloat(result.z))),
                asuint(saturate(asfloat(result.w))));
        uint dst = min((ins.x >> 8) & 15u, kAlphaInputs - 1u);
        uint mask = (ins.x >> 12) & 15u;
        uint4 old = ALPHA_LOAD(lane, dst);
        ALPHA_STORE(lane, dst, uint4((mask & 1u) != 0u ? result.x : old.x, (mask & 2u) != 0u ? result.y : old.y, (mask & 4u) != 0u ? result.z : old.z,
            (mask & 8u) != 0u ? result.w : old.w));
    }
    return true;
}
