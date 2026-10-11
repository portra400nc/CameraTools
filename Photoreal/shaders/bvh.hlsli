// The traced scene's traversal: a ray against core/bvh.h's nodes and triangles, nearest hit or any hit, with a stack of
// kStackSize entries per ray and a guard of kMaxSteps nodes. Shared by shaders/trace_cs.hlsl and, compiled as C++ through
// core/hlsl.h, by core/kernel.cpp, so it keeps to what core/hlsl.h supports.
//
// The includer defines, before including it:
//   uint4 node_row(uint node, uint row)            a node's 16-byte row 0 to 3
//   uint4 triangle_row(uint index, uint row)       a triangle's 16-byte row 0 to 2
//   STACK_STORE(lane, slot, value), STACK_LOAD(lane, slot)  the ray's stack, kStackSize entries per lane
//   LOOP                                           [loop] in HLSL

static const uint kStackSize = 24u;
static const uint kMaxSteps = 4096u;
static const uint kLeafLink = 0x80000000u;
static const uint kNoLink = 0xFFFFFFFFu;
static const uint kNoHit = 0xFFFFFFFFu;
static const float kMiss = 3.0e38f;
// Hits nearer than this along the ray are the surface the ray leaves.
static const float kTMin = 1.0e-4f;

struct BvhHit
{
    float t;            // the nearest hit so far, or the ray's end
    uint prim;          // its triangle, kNoHit for none
    float u;
    float v;
    uint steps;         // nodes visited
    uint overflow;      // 1 when a far child was left out because the stack was full
    uint guard;         // 1 when the ray stopped after kMaxSteps nodes
};

// 1 / d, with each component at least 1e-12 from 0 first, so a ray parallel to a slab gets a large finite inverse and
// a box test never multiplies 0 by infinity.
float3 safe_inverse(float3 d)
{
    float x = abs(d.x) > 1.0e-12f ? d.x : (d.x >= 0.0f ? 1.0e-12f : -1.0e-12f);
    float y = abs(d.y) > 1.0e-12f ? d.y : (d.y >= 0.0f ? 1.0e-12f : -1.0e-12f);
    float z = abs(d.z) > 1.0e-12f ? d.z : (d.z >= 0.0f ? 1.0e-12f : -1.0e-12f);
    return float3(1.0f / x, 1.0f / y, 1.0f / z);
}

// Where the ray enters the box within [0, t_max], or kMiss.
float box_entry(float3 lo, float3 hi, float3 origin, float3 inverse, float t_max)
{
    float3 a = (lo - origin) * inverse;
    float3 b = (hi - origin) * inverse;
    float enter = max(max(min(a.x, b.x), min(a.y, b.y)), max(min(a.z, b.z), 0.0f));
    float leave = min(min(max(a.x, b.x), max(a.y, b.y)), min(max(a.z, b.z), t_max));
    return enter <= leave ? enter : kMiss;
}

struct BvhTriangleData
{
    float3 v0;
    float3 e1;
    float3 e2;
    uint info;
};

BvhTriangleData load_bvh_triangle(uint index)
{
    uint4 r0 = triangle_row(index, 0u);
    uint4 r1 = triangle_row(index, 1u);
    uint4 r2 = triangle_row(index, 2u);
    BvhTriangleData t;
    t.v0 = float3(asfloat(r0.x), asfloat(r0.y), asfloat(r0.z));
    t.info = r0.w;
    t.e1 = float3(asfloat(r1.x), asfloat(r1.y), asfloat(r1.z));
    t.e2 = float3(asfloat(r2.x), asfloat(r2.y), asfloat(r2.z));
    return t;
}

// Moller-Trumbore, edges inclusive: the hit's t, or kMiss when it misses or lies outside (kTMin, t_max). A ray in the
// triangle's plane misses.
float4 intersect_triangle(BvhTriangleData tri, float3 origin, float3 direction, float t_max)
{
    float4 miss = float4(kMiss, 0.0f, 0.0f, 0.0f);
    float3 p = cross(direction, tri.e2);
    float det = dot(tri.e1, p);
    if (abs(det) < 1.0e-12f)
        return miss;
    float inverse = 1.0f / det;
    float3 s = origin - tri.v0;
    float u = dot(s, p) * inverse;
    float3 q = cross(s, tri.e1);
    float v = dot(direction, q) * inverse;
    float t = dot(tri.e2, q) * inverse;
    if (u >= 0.0f && v >= 0.0f && u + v <= 1.0f && t > kTMin && t < t_max)
        return float4(t, u, v, 0.0f);
    return miss;
}

// The triangles of a leaf, of the classes in mask (bits 1 << class), against the hit so far.
BvhHit visit_leaf(BvhHit hit, uint link, float3 origin, float3 direction, bool any_hit, uint mask)
{
    uint first = (link & 0x7FFFFFFFu) >> 4;
    uint count = (link & 15u) + 1u;
    LOOP for (uint i = 0u; i < count; i++)
    {
        BvhTriangleData tri = load_bvh_triangle(first + i);
        if (((mask >> ((tri.info >> 24) & 15u)) & 1u) == 0u)
            continue;
        float4 h = intersect_triangle(tri, origin, direction, hit.t);
        if (h.x < hit.t)
        {
            hit.t = h.x;
            hit.prim = first + i;
            hit.u = h.y;
            hit.v = h.z;
            if (any_hit)
                break;
        }
    }
    return hit;
}

// Depth first, the nearer internal child first, leaves as soon as their node is visited. any_hit stops at the first hit
// on a triangle of mask; otherwise the nearest is found, and boxes beyond it are left out.
BvhHit trace_bvh(float3 origin, float3 direction, float t_max, bool any_hit, uint mask, uint lane)
{
    BvhHit hit;
    hit.t = t_max;
    hit.prim = kNoHit;
    hit.u = 0.0f;
    hit.v = 0.0f;
    hit.steps = 0u;
    hit.overflow = 0u;
    hit.guard = 0u;
    float3 inverse = safe_inverse(direction);
    uint node = 0u;
    uint sp = 0u;
    LOOP for (;;)
    {
        if (hit.steps == kMaxSteps)
        {
            hit.guard = 1u;
            break;
        }
        hit.steps++;
        uint4 r0 = node_row(node, 0u);
        uint4 r1 = node_row(node, 1u);
        uint4 r2 = node_row(node, 2u);
        uint4 r3 = node_row(node, 3u);
        uint link0 = r3.x;
        uint link1 = r3.y;
        float t0 = link0 == kNoLink ? kMiss
            : box_entry(float3(asfloat(r0.x), asfloat(r0.y), asfloat(r0.z)), float3(asfloat(r0.w), asfloat(r1.x), asfloat(r1.y)), origin, inverse, hit.t);
        float t1 = link1 == kNoLink ? kMiss
            : box_entry(float3(asfloat(r1.z), asfloat(r1.w), asfloat(r2.x)), float3(asfloat(r2.y), asfloat(r2.z), asfloat(r2.w)), origin, inverse, hit.t);
        if (t0 != kMiss && (link0 & kLeafLink) != 0u)
        {
            hit = visit_leaf(hit, link0, origin, direction, any_hit, mask);
            if (any_hit && hit.prim != kNoHit)
                break;
        }
        if (t1 != kMiss && (link1 & kLeafLink) != 0u)
        {
            hit = visit_leaf(hit, link1, origin, direction, any_hit, mask);
            if (any_hit && hit.prim != kNoHit)
                break;
        }
        bool in0 = t0 != kMiss && (link0 & kLeafLink) == 0u && t0 <= hit.t;
        bool in1 = t1 != kMiss && (link1 & kLeafLink) == 0u && t1 <= hit.t;
        if (in0 && in1)
        {
            uint near_link = t0 <= t1 ? link0 : link1;
            uint far_link = t0 <= t1 ? link1 : link0;
            if (sp < kStackSize)
            {
                STACK_STORE(lane, sp, far_link);
                sp++;
            }
            else
                hit.overflow = 1u;
            node = near_link;
        }
        else if (in0)
            node = link0;
        else if (in1)
            node = link1;
        else
        {
            if (sp == 0u)
                break;
            sp--;
            node = STACK_LOAD(lane, sp);
        }
    }
    return hit;
}
