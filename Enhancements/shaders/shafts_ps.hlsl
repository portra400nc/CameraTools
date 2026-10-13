// The light-shaft pass's first draw, at half size: each ray from the game's camera is marched through the mist up to its
// pixel or shafts_distance, and the sun's visibility at each step comes from the game's sun shadow atlas, so shafts follow
// whatever casts the game's sun shadows, on screen or off. Each ray offsets its steps by the 4x4 pattern, so the raw
// result is noisy; light_shafts_ps.hlsl's 4x4 window averages it away as it brings the result to full size.
#include "sun.hlsli"
#include "scatter.hlsli"

Texture2D<float> atlas : register(t5);              // mirror of the game's sun atlas, copied at its shadow-mask draw
Texture3D<float> lattice : register(t6);            // the patch noise's random values, noise_lattice() in core/plan.h
SamplerComparisonState lit_compare : register(s2);  // bilinear, GREATER_EQUAL: lit where the point is at least as near
                                                    // the sun as what the atlas holds
SamplerState linear_wrap : register(s3);

struct Ray
{
    float4 mist : SV_Target0;   // rgb the light scattered toward the camera, a the share of what lies behind that comes through
    float meters : SV_Target1;  // the meters marched, which the second draw compares and rescales by
};

// The march ends once this little of what lies behind comes through.
static const float kOpaque = 1e-3;

// The patch noise's axes, turned away from the world's (an orthonormal matrix), so its lattice does not line up with
// walls and the horizon.
static const float3x3 kTurn = { 0.00, 0.80, 0.60, -0.80, 0.36, -0.48, -0.60, -0.48, 0.64 };
static const float kNoiseSize = 64;         // kNoiseSize in core/plan.h: the lattice repeats every 64 cells

// Value noise from one trilinear fetch: moving the sample point through its cell along smoothstep turns the hardware's
// linear filter into smoothstep interpolation between the lattice's values, as eight hashed corners would give.
float value_noise(float3 p)
{
    float3 cell = floor(p);
    float3 f = p - cell;
    return lattice.SampleLevel(linear_wrap, (cell + f * f * (3 - 2 * f) + 0.5) / kNoiseSize, 0);
}

// One octave only: a ray crosses dozens of patches, which average a finer octave away.
float patch_noise(float3 world) { return value_noise(mul(kTurn, world) / shafts_patch_size); }

Ray main(float4 position : SV_Position)
{
    int2 q = int2(position.xy);
    // The four half-size pixels of a 2x2 block look through the four full-size pixels of their cells, so a twig one pixel
    // wide still gets rays.
    int2 pixel = min(q * 2 + int2(q.y & 1, q.x & 1), int2(render_size.xy) - 1);
    float d = depth.Load(int3(pixel, 0));
    // The sky's far point may lie at infinity, so its direction comes from the near plane.
    float3 view = game_view_at((pixel + 0.5) * render_size.zw, d > 0 ? d : 1);
    float3 origin = game_b1[5].xyz;
    float3 dir = normalize(game_world(view) - origin);
    float ray_length = d > 0 ? min(length(view), shafts_distance) : shafts_distance;

    // The game lights a white surface facing the sun with sun_color, so the sun's irradiance is pi times it, and the sky's
    // radiance is sky_color, which the mist scatters evenly.
    float3 sun = dot(toward_sun, toward_sun) > 0 ? kPi * sun_color * shafts_strength * henyey_greenstein(dot(dir, toward_sun), shafts_anisotropy) : 0;
    float3 sky = sky_color * shafts_ambient;
    // Past the game's shadow distance, a view depth along its forward axis, the atlas holds nothing, and the mist is lit.
    float forward = dot(dir, game_b2[20].xyz);
    float shadowed_until = forward > 0 ? game_b0[10].x / forward : 1e30;
    // Where the ray lies in each cascade's sphere, in meters along it: from x to y, empty when it misses or the cascade is
    // not used. A step then takes the first cascade whose span holds it, as cascade_of picks the first sphere.
    float2 spans[kMaxCascades];
    [unroll] for (int k = 0; k < kMaxCascades; k++)
    {
        float3 offset = origin - game_b3[k].xyz;
        float b = dot(offset, dir);
        float discriminant = b * b - dot(offset, offset) + game_b3[k].w;
        float half_chord = sqrt(max(discriminant, 0));
        spans[k] = k < int(sun_cascades) && discriminant > 0 ? float2(-b - half_chord, -b + half_chord) : float2(1, 0);
    }

    Mist m;
    m.inscatter = 0;
    m.transmittance = 1;
    float n = shafts_steps;
    float jitter = pattern(q);
    // The cascade's transform is affine, so the atlas position is start + t * per_meter along the ray; both are made again
    // only when a step reaches another cascade, a few times per ray.
    int cascade = -1;
    float4 start = 0, per_meter = 0, tile = 0;
    [loop] for (uint i = 0; i < shafts_steps && m.transmittance > kOpaque; i++)
    {
        float t = march_at(i, n, jitter, ray_length);
        float3 p = origin + dir * t;
        float noise = 0.5;
        [branch] if (shafts_patchiness > 0)
            noise = patch_noise(p);
        float density = mist_density(shafts_density, dir.y * t, shafts_height, noise, shafts_patchiness);
        float visibility = 1;
        int c = -1;
        [unroll] for (int j = kMaxCascades - 1; j >= 0; j--)
        {
            if (t >= spans[j].x && t < spans[j].y)
                c = j;
        }
        [branch] if (c >= 0 && t < shadowed_until)
        {
            [branch] if (c != cascade)
            {
                cascade = c;
                CascadeTransform x = cascade_transform(c);
                float3 o = origin - x.origin;
                start = o.x * x.m0 + o.y * x.m1 + o.z * x.m2 + x.m3;
                per_meter = dir.x * x.m0 + dir.y * x.m1 + dir.z * x.m2;
                tile = cascade_tile(c);
            }
            float4 a = start + t * per_meter;
            // Raised by the atlas's 16-bit step toward lit, as receiver_depth is.
            visibility = atlas.SampleCmpLevelZero(lit_compare, clamp(a.xy / a.w, tile.xy, tile.zw), a.z / a.w + 2e-5);
        }
        m = mist_step(m, density, march_span(i, n, ray_length), mist_light(sun, visibility, sky));
    }
    Ray r;
    r.mist = float4(m.inscatter, m.transmittance);
    r.meters = ray_length;
    return r;
}
