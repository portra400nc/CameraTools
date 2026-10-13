// The traced scene's alpha test at a hit: one bilinear fetch from the alpha-tested triangle's coverage mask, compared with
// its cutoff. core/alpha_bake.cpp bakes each draw's alpha program into the masks and cutoffs when the scene is built, so
// the trace kernel runs no program. Shared by shaders/bvh.hlsli and, compiled as C++ through core/hlsl.h, by
// core/kernel.cpp and core/alpha_bake.cpp; it keeps to what core/hlsl.h supports.
//
// The includer defines:
//   uint alpha_word(uint index)            the alpha buffer's word
//   uint4 alpha_row(uint index)            its four words from index
//
// The mask buffer (core/alpha_bake.h), in 32-bit words: a header of 4 words, then 8 words a record, then the masks,
// 256x256 bytes each, from a 64 KB boundary of the buffer. A record: its mask's 64 KB slot in bits 0 to 11 of word 0 and
// the sampler's address modes in bits 12 to 17 (u, then v), the cutoff as a float in word 1, then the sample's
// coordinate at the triangle's three corners, u and v each.

static const uint kOpaqueRecord = 0xFFFFFFu;
static const uint kMaskHeaderWords = 4u;
static const uint kMaskRecordWords = 8u;
static const uint kMaskSide = 256u;

// A texel coordinate of a mask under a sampler's address mode (D3D11_TEXTURE_ADDRESS_MODE: 1 wrap, 2 mirror, 3 clamp,
// 4 border, 5 mirror once), or -1 outside a border. Without branches, so the kernel holds it small.
int mask_address(int i, uint mode)
{
    int m = i & 511;
    int repeated = mode == 2u ? m ^ ((m >> 8) * 511) : i & 255;
    int once = min(i ^ (i >> 31), 255);
    int border = i < 0 || i > 255 ? -1 : i;
    int clamped = min(max(i, 0), 255);
    return mode == 1u || mode == 2u ? repeated : mode == 5u ? once : mode == 4u ? border : clamped;
}

// The byte of the mask whose first byte is word first at texel (x, y), 0 outside a border.
uint mask_byte(uint first, int x, int y)
{
    uint at = uint(y) * kMaskSide + uint(x);
    uint word = alpha_word(first + at / 4u);
    return x < 0 || y < 0 ? 0u : (word >> ((at % 4u) * 8u)) & 255u;
}

// The mask bilinear at (u, v), with the sampler's address modes in bits 0 to 2 and 3 to 5: what the game's sample of the
// texture's level gives, as the program's interpreter (shaders/alpha.hlsli) samples it too.
float mask_sample(uint first, uint address, float u, float v)
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
    int xa = mask_address(ix, mu);
    int xb = mask_address(ix + 1, mu);
    int ya = mask_address(iy, mv);
    int yb = mask_address(iy + 1, mv);
    float top = lerp(float(mask_byte(first, xa, ya)), float(mask_byte(first, xb, ya)), fx);
    float bottom = lerp(float(mask_byte(first, xa, yb)), float(mask_byte(first, xb, yb)), fx);
    return lerp(top, bottom, fy) / 255.0f;
}

// Whether the alpha test keeps a hit at barycentric (u, v) of the triangle whose record is record: its coordinate,
// interpolated as the rasterizer interpolates the game's, samples its mask at least at the cutoff.
bool alpha_mask_keeps(uint record, float u, float v)
{
    uint4 a = alpha_row(kMaskHeaderWords + record * kMaskRecordWords);
    uint4 b = alpha_row(kMaskHeaderWords + record * kMaskRecordWords + 4u);
    float w = 1.0f - u - v;
    float x = asfloat(a.z) * w + asfloat(b.x) * u + asfloat(b.z) * v;
    float y = asfloat(a.w) * w + asfloat(b.y) * u + asfloat(b.w) * v;
    return mask_sample((a.x & 4095u) * 16384u, (a.x >> 12) & 63u, x, y) >= asfloat(a.y);
}
