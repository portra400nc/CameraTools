// The stars as points: each instance a star of the catalog's table, a quad over the 3x3 pixels around where it falls.
// Its light, dimmed and reddened by the air along its ray, spreads over them as a Gaussian that sums to 1 wherever the
// star lies between pixel centers, so a star neither flickers nor jumps while the camera turns.
#include "common.hlsli"
#include "sky.hlsli"

Buffer<float4> stars : register(t0);    // two a star: its J2000 direction, then its light in sun units

static const float kStarSpread = 0.6;  // the Gaussian's sigma, in pixels
static const float kMoonCover = 0.004528;

struct Star
{
    float4 position : SV_Position;
    nointerpolation float2 center : TEXCOORD0;     // where it falls, in pixels of the stored target
    nointerpolation float3 light : TEXCOORD1;      // its light over the pixels' solid angle and the spread's sum
};

Star main(uint vertex : SV_VertexID, uint instance : SV_InstanceID)
{
    Star o;
    o.position = float4(2, 2, 0, 1);
    o.center = 0;
    o.light = 0;
    float3 e = stars[instance * 2].xyz, flux = stars[instance * 2 + 1].rgb;
    float3 world = e.x * sky_stars_x + e.y * sky_stars_y + e.z * sky_stars_z;
    float3 view = mul((float3x3)world_to_view, world);
    if (world.y < -0.02 || view.z > -1e-3 || dot(world, sky_toward_moon) > cos(kMoonCover))
        return o;
    float4 clip = mul(view_to_clip, float4(view, 0));
    float2 ndc = clip.xy / clip.w;
    float2 p = stored_uv(float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5)) * render_size.xy;
    if (any(p < -2) || any(p > render_size.xy + 2))
        return o;
    float2 base = floor(p);
    float sum = 0;
    for (int y = -1; y <= 1; y++)
        for (int x = -1; x <= 1; x++)
        {
            float2 d = base + 0.5 + float2(x, y) - p;
            sum += exp(-dot(d, d) / (2 * kStarSpread * kStarSpread));
        }
    // A pixel's solid angle there: its width and height on the plane at distance 1, times the cube of the cosine off axis.
    float2 tangent = view.xy / -view.z;
    float cosine = rsqrt(1 + dot(tangent, tangent));
    float solid = 4 / (view_to_clip[0][0] * view_to_clip[1][1] * render_size.x * render_size.y) * cosine * cosine * cosine;
    float2 corner = base - 1 + 3 * float2(vertex & 1, vertex >> 1);
    o.position = float4(corner.x * render_size.z * 2 - 1, 1 - corner.y * render_size.w * 2, 0, 1);
    o.center = p;
    o.light = flux * sky_view_transmittance(world) * sky_scale / (solid * sum);
    return o;
}
