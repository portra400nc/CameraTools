// The physical sky's view tables: for each direction of kSkyViewSize's layout, the light the air between the viewer and
// the atmosphere's top, or the ground, scatters toward the viewer, as core/sky.cpp's sky_radiance marches it. The sun's and
// the moon's single scattering go in without their phase functions, which sky_ps.hlsl applies at each pixel's own angle;
// the multiple scattering and the lit ground, which have none, go in whole.
#include "common.hlsli"
#include "sky.hlsli"

struct SkyView
{
    float4 sun_rayleigh : SV_Target0;
    float4 sun_mie : SV_Target1;
    float4 moon_rayleigh : SV_Target2;
    float4 moon_mie : SV_Target3;
    float4 rest : SV_Target4;
};

SkyView main(float4 position : SV_Position)
{
    float3 view = sky_view_direction(position.xy / kSkyViewSize);
    float r0 = sky_viewer_radius();
    float ground = sky_distance_to_ground(r0, view.y);
    float span = ground >= 0 ? ground : sky_distance_to_top(r0, view.y);
    float3 sun = sky_toward_sun, moon = sky_toward_moon;
    float sun_e = 1, moon_e = sky_moon_illuminance;
    float3 throughput = 1, sun_r = 0, sun_m = 0, moon_r = 0, moon_m = 0, rest = 0;
    float previous = 0;
    const uint kSteps = 40;
    for (uint k = 1; k <= kSteps; k++)
    {
        float f = float(k) / kSteps, t = span * f * f, dt = t - previous;
        float3 p = float3(view.x * (previous + dt * 0.5), r0 + view.y * (previous + dt * 0.5), view.z * (previous + dt * 0.5));
        previous = t;
        float r = sqrt(dot(p, p));
        float3 up = p / r;
        SkyMedium m = sky_medium(r - kSkyGround);
        float mu_s = dot(up, sun), mu_m = dot(up, moon);
        float3 lit_s = sky_light_at(r, mu_s) * sun_e, lit_m = sky_light_at(r, mu_m) * moon_e;
        float3 psi = sky_multiple_at(r, mu_s) * sun_e + sky_multiple_at(r, mu_m) * moon_e;
        sun_r += throughput * sky_step(m.rayleigh * lit_s, m.extinction, dt);
        sun_m += throughput * sky_step(m.mie * lit_s, m.extinction, dt);
        moon_r += throughput * sky_step(m.rayleigh * lit_m, m.extinction, dt);
        moon_m += throughput * sky_step(m.mie * lit_m, m.extinction, dt);
        rest += throughput * sky_step((m.rayleigh + m.mie) * psi, m.extinction, dt);
        throughput *= exp(-m.extinction * dt);
    }
    if (ground >= 0)
    {
        float3 p = float3(view.x * ground, r0 + view.y * ground, view.z * ground);
        float3 up = normalize(p);
        float r = length(p);
        float mu_s = dot(up, sun), mu_m = dot(up, moon);
        rest += throughput * (sky_light_at(r, mu_s) * sun_e * max(mu_s, 0) + sky_light_at(r, mu_m) * moon_e * max(mu_m, 0)) * kSkyGroundAlbedo / kSkyPi;
    }
    SkyView o;
    o.sun_rayleigh = float4(sun_r, 1);
    o.sun_mie = float4(sun_m, 1);
    o.moon_rayleigh = float4(moon_r, 1);
    o.moon_mie = float4(moon_m, 1);
    o.rest = float4(rest, 1);
    return o;
}
