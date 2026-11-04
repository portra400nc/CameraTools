// The character light pass's first draw: each light's raw visibility at a character pixel, one channel a light, 1
// elsewhere. Each pixel marches toward the light through the depth buffer, its steps offset by the 4x4 pattern, so the
// raw result is noisy; character_light_ps.hlsl averages it over the pattern's 4x4 window.
#include "character_light.hlsli"

static const int kShadowSteps = 16;
// How far the march reaches toward a light, and how thick a surface it meets counts as solid.
static const float kShadowReach = 0.5;
static const float kShadowThickness = 0.25;

// 1 where nothing in the depth buffer lies between p and the light within reach, lower the nearer the blocker.
float visibility_toward(int2 pixel, float3 p, float3 n, float3 l, float reach)
{
    float3 origin = p + n * (-p.z / projection_scale);
    float step = min(reach, kShadowReach) / kShadowSteps;
    float offset = pattern(pixel);
    [loop] for (int i = 0; i < kShadowSteps; i++)
    {
        float t = (i + offset) * step;
        float3 ray = origin + l * t;
        if (ray.z >= -0.01)
            break;
        float2 st = stored_of(ray);
        if (any(st < 0) || any(st >= 1))
            break;
        int2 q = int2(st * render_size.xy);
        float d = depth.Load(int3(q, 0));
        if (d <= 0)
            continue;
        float in_front = view_position((q + 0.5) * render_size.zw, d).z - ray.z;
        if (in_front > 0.01 && in_front < kShadowThickness)
            return lerp(0.15, 1, (i / float(kShadowSteps)) * (i / float(kShadowSteps)));
    }
    return 1;
}

float4 main(float4 position : SV_Position) : SV_Target0
{
    int2 pixel = int2(position.xy);
    float4 seen = 1;
    if (depth.Load(int3(pixel, 0)) <= 0 || !is_character(stencil.Load(int3(pixel, 0)).g))
        return seen;
    float3 p = view_position_at(pixel);
    float3 n = character_normal(pixel, p, is_face(pixel));
    [unroll] for (uint k = 0; k < 4; k++)
    {
        if (k >= light_info.x)
            break;
        float4 r = local_reach_of(k, p);
        // A march toward a light behind the character runs behind its own visible surface, which then hides the light
        // wherever it is drawn: the shadow fades out as the light moves behind, where the falloff darkens the front.
        float trust = saturate(r.z * 4 + 1);
        if (r.w > 0 && dot(n, r.xyz) > -0.2 && trust > 0)
            seen[k] = lerp(1, visibility_toward(pixel, p, n, r.xyz, length(light_rows[k * 4u].xyz - p)), trust);
    }
    return seen;
}
