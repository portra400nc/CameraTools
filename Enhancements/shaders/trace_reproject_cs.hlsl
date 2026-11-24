// A live shot's move: each pixel of the new view takes the samples of the pixel that showed the same point of the scene
// to the camera they were traced with, where the old depth there agrees, and none where the point was hidden or off the
// old view. The occlusion and sun sums, the gather's hits and skies and the traced lights' sums move together, counts and
// all, so the trace and gather kernels go on adding to them. Reflections depend on the view: they move only from an
// anchored shot's anchor, whose copy is bound, and are cleared otherwise, as the unbound copy reads 0.

cbuffer Reproject : register(b0)
{
    float4 to_rows[4];      // core/trace.h's ReprojectConstants
    float4 from_rows[4];
    float4 from_clip[4];
    float4 eye;
    uint4 render;
};

StructuredBuffer<float4> old_history : register(t0);
StructuredBuffer<float4> old_hit : register(t1);
StructuredBuffer<float4> old_sky : register(t2);
StructuredBuffer<float4> old_lights : register(t3);
StructuredBuffer<float4> old_reflect : register(t6);
Texture2D<float> old_depth : register(t4);      // the depth the samples were traced over, reversed Z
Texture2D<float> new_depth : register(t5);
RWStructuredBuffer<float4> history : register(u0);
RWStructuredBuffer<float4> hit : register(u1);
RWStructuredBuffer<float4> sky : register(u2);
RWStructuredBuffer<float4> lights : register(u3);
RWStructuredBuffer<float4> reflect : register(u4);

// A stored pixel and its depth in the scene's frame, through a camera's clip rows, as trace_common.hlsli's scene_position.
float3 to_scene(float4 rows[4], int2 pixel, float depth)
{
    float u = (pixel.x + 0.5) / render.x, v = (pixel.y + 0.5) / render.y;
    if (render.z != 0)
        v = 1 - v;
    float4 clip = float4(u * 2 - 1, 1 - v * 2, depth, 1);
    return float3(dot(rows[0], clip), dot(rows[1], clip), dot(rows[2], clip)) / dot(rows[3], clip);
}

// An anchored shot's lens views see some of the scene the anchor did not: the ground behind a subject's outline, or
// another leaf of a blurred tree. Such a pixel takes the samples of the anchor's pixel nearby that showed the nearest
// point to its own, within a share of its distance, on a golden-angle spiral of kFillTaps out to kFillRadius pixels at
// 2160 rows: light varies smoothly along a surface, and left without samples the pixel showed the game's light, in a halo
// around the subject. render.w is 1 for an anchored shot's move.
static const uint kFillTaps = 48;
static const float kFillRadius = 48;
static const float kFillShare = 0.03;

uint fill_from(int2 q, float3 p)
{
    float radius = kFillRadius * render.y / 2160.0;
    float best = kFillShare * length(p - eye.xyz) + 0.05;
    uint found = 0xffffffff;
    for (uint i = 0; i < kFillTaps; i++)
    {
        float angle = float(i) * 2.39996323;
        float2 o = float2(cos(angle), sin(angle)) * (radius * sqrt((float(i) + 0.5) / float(kFillTaps)));
        int2 t = q + int2(round(o));
        if (any(t < 0) || t.x >= int(render.x) || t.y >= int(render.y))
            continue;
        float before = old_depth.Load(int3(t, 0));
        if (before <= 0)
            continue;
        float apart = length(to_scene(from_rows, t, before) - p);
        if (apart < best)
        {
            best = apart;
            found = uint(t.y) * render.x + uint(t.x);
        }
    }
    return found;
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    int2 pixel = int2(id.xy);
    if (pixel.x >= int(render.x) || pixel.y >= int(render.y))
        return;
    uint at = uint(pixel.y) * render.x + uint(pixel.x);
    float4 none = 0;
    float4 h = none, g = none, k = none, l = none, r = none;
    float d = new_depth.Load(int3(pixel, 0));
    if (d > 0)
    {
        float3 p = to_scene(to_rows, pixel, d);
        float4 c = float4(dot(from_clip[0], float4(p, 1)), dot(from_clip[1], float4(p, 1)), dot(from_clip[2], float4(p, 1)), dot(from_clip[3], float4(p, 1)));
        if (c.w > 0)
        {
            float2 ndc = c.xy / c.w;
            float u = ndc.x * 0.5 + 0.5, v = 0.5 - ndc.y * 0.5;
            if (render.z != 0)
                v = 1 - v;
            int2 q = int2(floor(float2(u * render.x, v * render.y)));
            uint from = 0xffffffff;
            if (all(q >= 0) && q.x < int(render.x) && q.y < int(render.y))
            {
                float before = old_depth.Load(int3(q, 0));
                float3 was = before > 0 ? to_scene(from_rows, q, before) : float3(1e30, 1e30, 1e30);
                if (length(was - p) <= eye.w * length(p - eye.xyz) + 0.02)
                    from = uint(q.y) * render.x + uint(q.x);
            }
            if (from == 0xffffffff && render.w != 0)
                from = fill_from(clamp(q, int2(0, 0), int2(render.xy) - 1), p);
            if (from != 0xffffffff)
            {
                h = old_history[from];
                g = old_hit[from];
                k = old_sky[from];
                l = old_lights[from];
                r = old_reflect[from];
            }
        }
    }
    history[at] = h;
    hit[at] = g;
    sky[at] = k;
    lights[at] = l;
    reflect[at] = r;
}
