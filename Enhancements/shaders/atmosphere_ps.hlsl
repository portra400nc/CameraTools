// The atmosphere pass: aerial perspective over the HDR scene, before the game's bloom and tone map read it. Writes the
// whole target, because the result is copied over the game's HDR scene whole. The sky (depth 0) takes the haze along its
// ray up to kSkyReach: kept clear, it met a thick weather mist on the hills behind at a hard, bright edge.
// With mist_patchiness the haze near the camera gathers into banks that drift with the wind: the ray is marched through
// them up to a few banks away, and the rest of it takes the even haze, which the banks match on average.
#include "gbuffer.hlsli"
#include "scatter.hlsli"

Texture2D<float3> scene : register(t0);     // mirror of the game's HDR scene
Texture3D<float> lattice : register(t6);    // the noise's random values, noise_lattice() in core/plan.h
SamplerState linear_wrap : register(s3);

static const float kNoiseSize = 64;         // kNoiseSize in core/plan.h
static const float3x3 kTurn = { 0.00, 0.80, 0.60, -0.80, 0.36, -0.48, -0.60, -0.48, 0.64 };
static const uint kSteps = 12;
static const float kReachBanks = 8;         // banks marched along a ray before the even haze takes over
static const float kSkyReach = 20000;       // meters of haze in front of the sky

// The integral of exp(-height_falloff * rise * t) dt for t from 0 to distance: the ray's length weighted by the haze's
// density relative to the camera's height. rise is the ray's upward component per meter.
float haze_length(float distance, float rise)
{
    float x = max(height_falloff * rise * distance, -80);   // far below the camera the haze is opaque anyway
    if (abs(x) < 1e-3)
        return distance * (1 - 0.5 * x);
    return distance * (1 - exp(-x)) / x;
}

float value_noise(float3 p)
{
    float3 cell = floor(p);
    float3 f = p - cell;
    return lattice.SampleLevel(linear_wrap, (cell + f * f * (3 - 2 * f) + 0.5) / kNoiseSize, 0);
}

// How much of the even haze's density a point of the world holds: 1 everywhere at patchiness 0; at 1, banks of about
// twice it with clear air between, flatter than they are wide, so they lie along the ground. Its mean stays near 1.
float bank_share(float3 world)
{
    float3 p = float3(world.x + mist_drift.x, world.y * 2.5, world.z + mist_drift.y) / mist_size;
    float n = 0.65 * value_noise(mul(kTurn, p)) + 0.35 * value_noise(mul(kTurn, p * 2.7) + 19);
    return lerp(1, 2 * smoothstep(0.32, 0.68, n), mist_patchiness);
}

float3 main(float4 position : SV_Position) : SV_Target0
{
    int2 pixel = int2(position.xy);
    float3 color = scene.Load(int3(pixel, 0));
    float d = depth.Load(int3(pixel, 0));
    float3 p = view_position((pixel + 0.5) * render_size.zw, d > 0 ? d : 1e-6);
    float3 ray = normalize(p);
    float distance = d > 0 ? length(p) : kSkyReach;
    float3 world_ray = mul(ray, (float3x3)world_to_view);   // world_to_view is orthogonal, so its transpose takes view to world
    float rise = world_ray.y;
    float optical = haze_length(distance, rise);
    if (mist_patchiness > 0)
    {
        float3 translation = float3(world_to_view._m03, world_to_view._m13, world_to_view._m23);
        float3 camera = mul(-translation, (float3x3)world_to_view);
        float reach = min(distance, mist_size * kReachBanks);
        // Each pixel starts its steps at a different place along them, which turns the steps' banding into fine grain.
        float jitter = frac(52.9829189 * frac(dot(position.xy, float2(0.06711056, 0.00583715))));
        float step = reach / kSteps, banked = 0;
        [unroll] for (uint i = 0; i < kSteps; i++)
        {
            float t = (i + jitter) * step;
            banked += exp(-height_falloff * rise * t) * bank_share(camera + world_ray * t) * step;
        }
        optical = banked + optical - haze_length(reach, rise);
    }
    float transmittance = exp(-atmosphere_density * optical);
    float3 l = mul((float3x3)world_to_view, toward_sun);
    float3 sun = dot(l, l) > 0 ? sun_color * sun_scatter * henyey_greenstein(dot(ray, l), anisotropy) : 0;
    return color * transmittance + (sky_color + sun) * (1 - transmittance);
}
