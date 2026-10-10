// The leaves pass: the sun light leaves and grass let through toward the camera, drawn additively into a mirror of the HDR
// scene after the game's deferred lighting. Every other pixel adds nothing.
#include "sun.hlsli"
#include "leaves.hlsli"

Texture2D<float2> shadow_mask : register(t0);   // mirror of the game's shadow mask; the sun's visibility in .x
Texture2D<float4> albedo : register(t5);        // mirror of G-buffer rt1, sRGB-encoded as stored
Texture2D<float> material : register(t6);       // mirror of G-buffer rt3: the material id / 255, with flags in bits 6 and 7
Texture2D<float> atlas : register(t7);          // mirror of the game's sun atlas, read only while sun_cascades is not 0

static const int kTaps = 8;
// The taps' radius across the light, in meters: wide enough to average the holes of single alpha-tested leaves out of
// the atlas, and narrow enough that a slanted leaf barely shades itself, since the taps compare against a flat plane.
static const float kSpread = 0.1;

float3 srgb_to_linear(float3 c) { return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4); }

// The share of sun light that reaches the leaf through the foliage between it and the sun, from the atlas over a disc
// turned by the 4x4 pattern. Where the atlas cannot say, without this frame's constants, past the game's shadow distance
// or outside every cascade, the sun's visibility in the shadow mask.
float sun_through_leaves(int2 pixel)
{
    float game = shadow_mask.Load(int3(pixel, 0)).x;
    if (sun_cascades == 0)
        return game;
    float3 view = game_view(pixel);
    if (-view.z >= game_b0[10].x)
        return game;
    float3 p = game_world(view);
    float edge, next_edge;
    int c = cascade_of(p, 0, edge);
    if (c < 0)
        return game;
    // In a cascade's outer band, a growing share of pixels take the next cascade, so the seam between them dithers.
    if (edge > pattern(pixel.yx) && cascade_of(p, c + 1, next_edge) == c + 1)
        c++;
    // A flat plane: a leaf's neighbors in the atlas are other leaves, which the plane through the leaf would not predict.
    Receiver r = project(c, p, 0);
    float radius = max(kSpread, game_b0[0].x / r.uv_per_meter.x);   // at least a texel, so far cascades still spread
    float turn = pattern(pixel);
    float sum = 0;
    [unroll] for (int i = 0; i < kTaps; i++)
    {
        float2 uv = tap(r, i, kTaps, turn, radius);
        float stored = atlas.SampleLevel(point_clamp, uv, 0);
        sum += leaf_transmission(leaf_thickness(stored, receiver_depth(r, uv), r.meters_per_depth), leaves_depth);
    }
    return sum / kTaps;
}

float3 main(float4 position : SV_Position) : SV_Target0
{
    int2 pixel = int2(position.xy);
    uint id = uint(material.Load(int3(pixel, 0)) * 255 + 0.5) & ~0xC0u;
    // Leaves are ids 2 and 15, grass 3, as the game's vegetation lighting draw tests them.
    if (!is_foliage(stencil.Load(int3(pixel, 0)).g) || (id != 2 && id != 15 && id != 3))
        return 0;
    float3 l = mul((float3x3)world_to_view, toward_sun);
    float3 toward_pixel = normalize(view_position_at(pixel));
    float3 n = view_normal_at(pixel);
    // Bright where the camera looks toward the sun, and only on the side of the leaf away from it: the game's wrapped
    // diffuse already lights back faces dimly and uncolored, so front-lit leaves get nothing more.
    float forward_scatter = pow(saturate(dot(toward_pixel, l)), leaves_scatter_sharpness) * saturate(-dot(n, l) * 0.5 + 0.5);
    // Most leaves on screen face away from the sun's glow, and they skip the atlas.
    if (forward_scatter <= 0)
        return 0;
    return srgb_to_linear(albedo.Load(int3(pixel, 0)).rgb) * sun_color * sun_through_leaves(pixel) * leaves_strength * forward_scatter;
}
