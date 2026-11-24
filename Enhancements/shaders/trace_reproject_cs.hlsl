// A live shot's move: each pixel of the new view takes the samples of the pixel that showed the same point of the scene
// to the camera they were traced with, where the old depth there agrees, and none where the point was hidden or off the
// old view. The occlusion and sun sums, the gather's hits and skies and the traced lights' sums move together, counts and
// all, so the trace and gather kernels go on adding to them. Reflections depend on the view: they move only from an
// anchored shot's anchor, whose copy is bound, and are cleared otherwise, as the unbound copy reads 0. An anchored move
// takes the pixels its anchor did not see from the view before it, whose own move traced them, so each pixel hidden from
// the anchor is traced once while the views that cannot see it from the anchor go on, and adds samples in each.

cbuffer Reproject : register(b0)
{
    float4 to_rows[4];      // core/trace.h's ReprojectConstants
    float4 from_rows[4];
    float4 from_clip[4];
    float4 last_rows[4];    // an anchored move's view before it: its clip rows and scene to clip
    float4 last_clip[4];
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
StructuredBuffer<float4> last_history : register(t7);
StructuredBuffer<float4> last_hit : register(t8);
StructuredBuffer<float4> last_sky : register(t9);
StructuredBuffer<float4> last_lights : register(t10);
StructuredBuffer<float4> last_reflect : register(t11);
Texture2D<float> last_depth : register(t12);
// render.w is 1 for an anchored shot's move, which lists each world pixel its anchor did not see, for its pass to trace
// alone (work_pixel.hlsli): their count, then each as x | y << 16. The indirect dispatch's second argument counts a
// group row for each 4096 of them.
RWBuffer<uint> hole_list : register(u5);
RWBuffer<uint> hole_args : register(u6);

// A stored pixel and its depth in the scene's frame, through a camera's clip rows, as trace_common.hlsli's scene_position.
float3 to_scene(float4 rows[4], int2 pixel, float depth)
{
    float u = (pixel.x + 0.5) / render.x, v = (pixel.y + 0.5) / render.y;
    if (render.z != 0)
        v = 1 - v;
    float4 clip = float4(u * 2 - 1, 1 - v * 2, depth, 1);
    return float3(dot(rows[0], clip), dot(rows[1], clip), dot(rows[2], clip)) / dot(rows[3], clip);
}

// The stored pixel a camera's scene to clip shows p at, or -1 off its view.
int2 seen_at(float4 clip_of[4], float3 p)
{
    float4 c = float4(dot(clip_of[0], float4(p, 1)), dot(clip_of[1], float4(p, 1)), dot(clip_of[2], float4(p, 1)), dot(clip_of[3], float4(p, 1)));
    if (c.w <= 0)
        return int2(-1, -1);
    float2 ndc = c.xy / c.w;
    float u = ndc.x * 0.5 + 0.5, v = 0.5 - ndc.y * 0.5;
    if (render.z != 0)
        v = 1 - v;
    int2 q = int2(floor(float2(u * render.x, v * render.y)));
    return all(q >= 0) && q.x < int(render.x) && q.y < int(render.y) ? q : int2(-1, -1);
}

// Whether the point a camera stored at q over depth before is p, within the share of p's distance eye.w allows.
bool same_point(float4 rows[4], int2 q, float before, float3 p)
{
    float3 was = before > 0 ? to_scene(rows, q, before) : float3(1e30, 1e30, 1e30);
    return length(was - p) <= eye.w * length(p - eye.xyz) + 0.02;
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
        int2 q = seen_at(from_clip, p);
        if (q.x >= 0 && same_point(from_rows, q, old_depth.Load(int3(q, 0)), p))
        {
            uint from = uint(q.y) * render.x + uint(q.x);
            h = old_history[from];
            g = old_hit[from];
            k = old_sky[from];
            l = old_lights[from];
            r = old_reflect[from];
        }
        if (render.w != 0 && h.z == 0)
        {
            q = seen_at(last_clip, p);
            if (q.x >= 0 && same_point(last_rows, q, last_depth.Load(int3(q, 0)), p))
            {
                uint from = uint(q.y) * render.x + uint(q.x);
                h = last_history[from];
                g = last_hit[from];
                k = last_sky[from];
                l = last_lights[from];
                r = last_reflect[from];
            }
        }
        if (render.w != 0 && h.z == 0)
        {
            uint slot;
            InterlockedAdd(hole_list[0], 1u, slot);
            hole_list[slot + 1u] = uint(pixel.x) | (uint(pixel.y) << 16);
            if (slot % 4096u == 0u)
                InterlockedAdd(hole_args[1], 1u);
        }
    }
    history[at] = h;
    hit[at] = g;
    sky[at] = k;
    lights[at] = l;
    reflect[at] = r;
}
