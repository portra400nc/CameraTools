// The physical sky's night on vegetation (stencil 129, 136 and 137), for sky_ps.hlsl: the game lights it in a deferred
// pass of its own with a light that does not follow the sky, so a vegetation pixel keeps sky_vegetation of the game's
// light, and the rest goes to what the game's lamps near the camera (core/night_lamps.h) shed on it, by the game's own law
// (shaders/local_light.hlsli): its albedo times each lamp's radiance, 1 / (1 + 25 x^2) of its range, faded over its last
// fifth, a spot's cone, and the leaf's facing wrapped, since light passes through leaves. Include after common.hlsli.

Texture2D<uint2> vegetation_stencil : register(t11);    // the depth mirror's stencil in .g
Texture2D<float4> vegetation_normals : register(t12);   // G-buffer rt0, world normal * 0.5 + 0.5
Texture2D<float4> vegetation_albedo : register(t13);    // G-buffer albedo, sRGB

cbuffer NightLamps : register(b4)
{
    float4 lamp_rows[128];      // per lamp: view position and range, radiance and cos_outer, view direction and cos_inner, unused
    uint4 lamp_info;            // x: the count
};

float3 vegetation_linear(float3 c) { return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4); }

float3 night_vegetation(float3 color, int2 pixel, float3 at)
{
    if (sky_vegetation >= 1 || !is_foliage(vegetation_stencil.Load(int3(pixel, 0)).g))
        return color;
    float3 n = normalize(mul((float3x3)world_to_view, vegetation_normals.Load(int3(pixel, 0)).xyz * 2 - 1));
    float3 shed = 0;
    for (uint k = 0; k < min(lamp_info.x, 32u); k++)
    {
        float4 place = lamp_rows[k * 4u], radiance = lamp_rows[k * 4u + 1u], axis = lamp_rows[k * 4u + 2u];
        float3 v = place.xyz - at;
        float d = max(length(v), 1e-3), x = d / place.w;
        if (x >= 1)
            continue;
        float3 l = v / d;
        float spot = saturate((-dot(l, axis.xyz) - radiance.w) / max(axis.w - radiance.w, 1e-4));
        shed += radiance.rgb * saturate(dot(n, l) * 0.6 + 0.4) * saturate((1 - x) * 5) * spot / (1 + 25 * x * x);
    }
    return color * sky_vegetation + (1 - sky_vegetation) * vegetation_linear(vegetation_albedo.Load(int3(pixel, 0)).rgb) * shed;
}
