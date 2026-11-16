// The clouds' sunlight and its scattering, shared by the march (clouds_ps.hlsl) and the painted layer (clouds_painted_ps.hlsl).
// Include it after clouds.hlsli, scatter.hlsli and sky.hlsli.

// Multiple scattering after Wrenninge: each octave lets more light through, scatters less and more evenly.
float scattered(float c, float od)
{
    float lit = 0, a = 1, b = 1, g = 1;
    [unroll] for (int k = 0; k < 4; k++)
    {
        lit += b * (0.6 * henyey_greenstein(c, 0.8 * g) + 0.4 * henyey_greenstein(c, -0.15 * g)) * exp(-od * a);
        a *= 0.25;
        b *= 0.45;
        g *= 0.6;
    }
    return lit;
}

// With the physical sky, the light along cloud_toward_sun that reaches world point p through the air above it, on the
// curved planet below the viewer at origin: clouds stay lit after the sun has set on the ground. The stylized sunset
// lights them as if they stood up to kSunsetLiftKm higher, so they keep the low sun's pink longer after sunset.
static const float kSunsetLiftKm = 8;
float3 physical_sunlight(float3 origin, float3 p)
{
    float lift = sky_sunset * kSunsetLiftKm;
    float3 planet = float3((p.x - origin.x) * 0.001, sky_viewer_radius() + (p.y - origin.y) * 0.001 + lift, (p.z - origin.z) * 0.001);
    float r = length(planet);
    return sky_scale * sky_cloud_illuminance * sky_light_at(r, dot(planet / r, cloud_toward_sun)) * cloud_sun;
}

// The same at the layer's middle where the ray crosses it, for the thin layers lit once a ray.
float3 layer_sunlight(float3 origin, float3 dir)
{
    float middle = cloud_base + 0.5 * cloud_thickness;
    float t = abs(dir.y) > 0.01 ? max((middle - origin.y) / dir.y, 0) : 0;
    return physical_sunlight(origin, origin + dir * min(t, cloud_map_extent * 0.35));
}

// The stylized sunset's clouds: sunlight the air reddened lights them up to three times as brightly, so they glow pink
// against the sky as the low sun catches them.
float3 sunset_glow(float3 sun)
{
    float warmth = saturate(1 - sun.b / max(sun.r, 1e-6));
    return sun * (1 + 2 * sky_sunset * warmth);
}
