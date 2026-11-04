// A traced light's light on a surface, as Genshin lights one with a Unity Light (enhancements.h, EnhancementsShotLight):
// its radiance, which is its color times its intensity to the power 2.2 and already in light_rows, times max(n.l, 0),
// 1 / (1 + 25 x^2) with x its distance over its range, a fade over the last fifth of its range, and for a spot a linear
// ramp in the cosine from its outer cone to its inner one. The includer defines the GatherLights constants of
// core/trace.h: float4 light_rows[16], four rows a light; uint4 light_info, x the count.
#ifndef LOCAL_LIGHT_HLSLI
#define LOCAL_LIGHT_HLSLI

// The unit direction toward the light from p, and in w its reach there before the surface's facing.
float4 local_reach_of(uint k, float3 p)
{
    float4 at = light_rows[k * 4u];
    float cos_outer = light_rows[k * 4u + 1u].w;
    float4 axis = light_rows[k * 4u + 2u];
    float3 v = float3(at.x - p.x, at.y - p.y, at.z - p.z);
    float d = max(length(v), 1.0e-3f);
    float3 l = v * (1.0f / d);
    float x = d / at.w;
    // A point light's cosines are -2, which makes its ramp 1 wherever it shines.
    float spot = saturate((-dot(l, float3(axis.x, axis.y, axis.z)) - cos_outer) / max(axis.w - cos_outer, 1.0e-4f));
    return float4(l.x, l.y, l.z, saturate((1.0f - x) * 5.0f) * spot / (1.0f + 25.0f * x * x));
}

// The light's geometry at p of normal n: what multiplies its radiance.
float local_geometry_of(uint k, float3 p, float3 n)
{
    float4 r = local_reach_of(k, p);
    return max(dot(n, float3(r.x, r.y, r.z)), 0.0f) * r.w;
}

float3 local_light_of(uint k, float3 p, float3 n)
{
    float4 radiance = light_rows[k * 4u + 1u];
    return float3(radiance.x, radiance.y, radiance.z) * local_geometry_of(k, p, n);
}

#endif
