// The cloud layer's second draw, at half size: each ray from the game's camera crosses the slab between cloud_base and
// cloud_base + cloud_thickness in cloud_steps samples, each lit by the main light through a short march toward it and by
// the sky. The result depends on the ray's direction alone, so clouds_apply_ps.hlsl filters it to full size.
#include "sun.hlsli"
#include "clouds.hlsli"
#include "scatter.hlsli"

Texture2D<float> cloud_map : register(t5);

float cloud_shape(float3 p, out float h)
{
    h = saturate((p.y - cloud_base) / cloud_thickness);
    float2 uv = (p.xz + cloud_wind - cloud_map_center) / cloud_map_extent + 0.5;
    float d = cloud_map.SampleLevel(linear_clamp, uv, 0);
    return d - (2 * h - 1) * (2 * h - 1) * 0.6;
}

// Extinction per meter at p, with the fine noise eroding the edges.
float cloud_extinction(float3 p, out float h)
{
    float dome = cloud_shape(p, h);
    if (dome <= 0 || p.y < cloud_base || p.y > cloud_base + cloud_thickness)
        return 0;
    float f = cloud_size * 0.12;
    float detail = cloud_fbm(float3(p.x + cloud_wind.x, p.y, p.z + cloud_wind.y) / f, 2);
    dome -= cloud_detail * (1 - detail) * (1 - saturate(dome)) * 0.6;
    return pow(saturate(dome), 1.2) * cloud_density;
}

// The same without the fine noise, for the march toward the light.
float cloud_extinction_coarse(float3 p)
{
    float h;
    float dome = cloud_shape(p, h);
    if (p.y < cloud_base || p.y > cloud_base + cloud_thickness)
        return 0;
    return pow(saturate(dome), 1.2) * cloud_density;
}

float4 main(float4 position : SV_Position) : SV_Target0
{
    float2 uv = position.xy * 2 * render_size.zw;
    float3 origin = game_b1[5].xyz;
    float3 dir = normalize(game_world(game_view_at(uv, 1)) - origin);
    float top = cloud_base + cloud_thickness;
    if (dir.y < 0.01 || origin.y > top)
        return float4(0, 0, 0, 1);
    float t0 = max((cloud_base - origin.y) / dir.y, 0);
    float t1 = min((top - origin.y) / dir.y, t0 + cloud_thickness * 6);
    float dt = (t1 - t0) / cloud_steps;
    float jitter = frac(52.9829189 * frac(dot(position.xy, float2(0.06711056, 0.00583715))));

    float3 l = cloud_toward_sun;
    float c = dot(dir, l);
    float light_length = cloud_thickness / max(l.y, 0.15) * 0.6;
    float3 sun = kPi * sun_color * cloud_sun;
    float3 sky = sky_color * cloud_ambient;

    float3 inscatter = 0;
    float transmittance = 1;
    for (uint i = 0; i < cloud_steps && transmittance > 0.01; i++)
    {
        float3 p = origin + dir * (t0 + (i + jitter) * dt);
        float h;
        float sigma = cloud_extinction(p, h);
        if (sigma <= 0)
            continue;
        float depth_to_light = 0;
        [unroll] for (int j = 0; j < 4; j++)
            depth_to_light += cloud_extinction_coarse(p + l * (light_length * (j + 0.5) / 4)) * light_length / 4;
        // Multiple scattering after Wrenninge: each octave lets more light through, scatters less and more evenly.
        float lit = 0, a = 1, b = 1, g = 1;
        [unroll] for (int k = 0; k < 3; k++)
        {
            lit += b * (0.6 * henyey_greenstein(c, 0.7 * g) + 0.4 * henyey_greenstein(c, -0.2 * g)) * exp(-depth_to_light * a);
            a *= 0.3;
            b *= 0.45;
            g *= 0.5;
        }
        float powder = 1 - exp(-sigma * 2 * dt);
        float3 light = sun * lit * (0.5 + 0.5 * powder) + sky * (0.12 + 0.88 * pow(h, 0.7));
        float through = exp(-sigma * dt);
        inscatter += transmittance * (1 - through) * light;
        transmittance *= through;
    }
    float fade = exp(-t0 / cloud_fade);
    return float4(inscatter * fade, 1 - (1 - transmittance) * fade);
}
