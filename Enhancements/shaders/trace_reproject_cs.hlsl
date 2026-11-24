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
            if (all(q >= 0) && q.x < int(render.x) && q.y < int(render.y))
            {
                float before = old_depth.Load(int3(q, 0));
                float3 was = before > 0 ? to_scene(from_rows, q, before) : float3(1e30, 1e30, 1e30);
                if (length(was - p) <= eye.w * length(p - eye.xyz) + 0.02)
                {
                    uint from = uint(q.y) * render.x + uint(q.x);
                    h = old_history[from];
                    g = old_hit[from];
                    k = old_sky[from];
                    l = old_lights[from];
                    r = old_reflect[from];
                }
            }
        }
    }
    history[at] = h;
    hit[at] = g;
    sky[at] = k;
    lights[at] = l;
    reflect[at] = r;
}
