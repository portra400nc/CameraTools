// The physical sky over the sky's pixels (depth 0), sky_drawn.hlsli's, unless sky_dome_ps.hlsl drew it in place of the
// game's dome this frame; and over the scene's pixels the air between them and the viewer. Writes every pixel, because
// the result is copied over the game's HDR scene whole.
#include "common.hlsli"
#include "sky.hlsli"
#include "vegetation.hlsli"
#include "sky_drawn.hlsli"

Texture2D<float3> scene : register(t0);             // mirror of the game's HDR scene
Texture2D<float> depth : register(t2);              // mirror of the main depth, reversed Z
Texture2D<float> aerial_lit : register(t8);         // aerial_lit_ps.hlsl's share of lit air, half size, while sky_aerial_lit

// The optical depth over km kilometers along a ray of cosine mu from the viewer's height, of a layer of scale height H, over
// its density at the ground.
float layer_depth(float km, float mu, float H)
{
    float rise = mu * km;
    return exp(-sky_camera_km / H) * (abs(rise) > 1e-4 * H ? H * (1 - exp(-rise / H)) / mu : km);
}

// Aerial perspective: the scene point's light through the air between, sky_aerial times as far as it is, and the light
// that air scatters toward the viewer, taken at its middle height: the sun's and the moon's light that reaches it (the
// planet's shadow included) scattered once at each phase, lit of it where no terrain shades it, and the light scattered
// again.
float3 aerial(float3 color, float meters, float3 view, float lit)
{
    float km = meters * 0.001 * sky_aerial, mu = view.y;
    float3 thickness = kSkyRayleigh * layer_depth(km, mu, kSkyRayleighHeight) + (sky_haze / kSkyMieHeight) * layer_depth(km, mu, kSkyMieHeight);
    float3 through = exp(-thickness);
    float height = max(sky_camera_km + mu * km * 0.5, 0), r = kSkyGround + height;
    SkyMedium m = sky_medium(height);
    float cs = dot(view, sky_toward_sun), cm = dot(view, sky_toward_moon);
    float3 sun = sky_light_at(r, sky_toward_sun.y), moon = sky_light_at(r, sky_toward_moon.y) * sky_moon_illuminance;
    float3 once = sun * (m.rayleigh * sky_rayleigh_phase(cs) + m.mie * sky_mie_phase(cs))
        + moon * (m.rayleigh * sky_rayleigh_phase(cm) + m.mie * sky_mie_phase(cm));
    float3 again = (m.rayleigh + m.mie) * (sky_multiple_at(r, sky_toward_sun.y) + sky_multiple_at(r, sky_toward_moon.y) * sky_moon_illuminance);
    // Under an overcast the clouds take most of the sunlight the air would scatter, and the rest grays.
    float3 inscatter = (lit * once * (1 - 0.9 * sky_overcast) + again * (1 - 0.5 * sky_overcast)) * (1 - through) / max(m.extinction, 1e-6);
    inscatter = lerp(inscatter, dot(inscatter, float3(0.2126, 0.7152, 0.0722)), sky_overcast);
    return color * through + inscatter * sky_scale;
}

float3 main(float4 position : SV_Position) : SV_Target0
{
    int2 pixel = int2(position.xy);
    float3 color = scene.Load(int3(pixel, 0));
    float scene_depth = depth.Load(int3(pixel, 0));
    if (scene_depth > 0)
    {
        float3 at = view_position(position.xy * render_size.zw, scene_depth);
        color = night_vegetation(color, pixel, at);
        if (sky_aerial <= 0)
            return color;
        float3 toward = normalize(mul(normalize(at), (float3x3)world_to_view));
        float lit = sky_aerial_lit > 0.5 ? aerial_lit.Load(int3(pixel / 2, 0)) : 1;
        return min(aerial(color, length(at), toward, lit), kMostLight);
    }
    // Drawn in place of the game's dome this frame, under what the game drew over it since.
    if (sky_dome > 0.5)
        return color;
    return drawn_sky(position.xy);
}
