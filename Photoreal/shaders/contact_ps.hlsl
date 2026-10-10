// The contact-shadow pass's first draw: raw sun visibility into our render-size scratch, 1 on pixels that are not world.
// Each world pixel marches from its surface toward the sun through the depth buffer, its steps offset by the 4x4
// pattern, so the raw result is noisy; contact_shadows_ps.hlsl's 4x4 blur averages it away.
#include "gbuffer.hlsli"

static const int kSteps = 16;

// 1 where nothing in the depth buffer lies between the surface and the sun within contact_length, 0 where something
// does near the surface, fading back to 1 for hits in the far half of the ray, so the ray's end draws no edge.
float sun_visibility(int2 pixel, float3 p, float3 n, float3 l)
{
    // The ray starts one pixel's footprint off the surface. Depth read at the nearest pixel center differs from the
    // surface under the ray by at most half a footprint along the normal, so the surface never shadows itself.
    float3 origin = p + n * (-p.z / projection_scale);
    float step = contact_length / kSteps;
    float offset = pattern(pixel);
    [loop] for (int i = 0; i < kSteps; i++)
    {
        float t = (i + offset) * step;
        float3 ray = origin + l * t;
        if (ray.z >= -0.01)
            break;                          // behind the camera
        float2 st = stored_of(ray);
        if (any(st < 0) || any(st >= 1))
            break;                          // off screen: nothing known
        int2 q = int2(st * render_size.xy);
        float d = depth.Load(int3(q, 0));
        if (d <= 0)
            continue;                       // sky
        float in_front = view_position((q + 0.5) * render_size.zw, d).z - ray.z;
        if (in_front > 0 && in_front < contact_thickness)
            return saturate(2 * t / contact_length - 1);
    }
    return 1;
}

float main(float4 position : SV_Position) : SV_Target0
{
    int2 pixel = int2(position.xy);
    if (!is_world(stencil.Load(int3(pixel, 0)).g))
        return 1;
    float3 n = view_normal_at(pixel);
    float3 l = mul((float3x3)world_to_view, toward_sun);
    if (dot(n, l) <= 0)
        return 1;                           // facing away from the sun, which the game's lighting already darkens, or no sun
    return sun_visibility(pixel, view_position_at(pixel), n, l);
}
