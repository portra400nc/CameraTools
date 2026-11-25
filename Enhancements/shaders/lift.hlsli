// The highlight lift of lens-sampled shots. The game's HDR scene is shallow: a lamp, a lantern or a gold ornament stands
// only a few times above what is around it, where a real one, much brighter, keeps its disc when blurred. Each sample's
// out-of-focus light is multiplied by how far it stands above its own surroundings before its gap fill and the sum: by
// 1 + strength (ratio - threshold), at most 1 + 2 strength, where ratio is the mean of a small block around the pixel
// over the mean of the 7 x 7 blocks around that, so a whole lantern takes one gain and its rim no more than its middle.
// The gain fades in with the lens's CoC from a quarter of a 400-row picture's pixel to one, so what is in focus stays
// as the game drew it, and a wider disc spreads the lifted light thinner, as a real lens's does.
// (.work/dof/lift/render_frame.py tried it on a saved frame against an exposure lift and an expansion curve.)
#ifndef LIFT_HLSLI
#define LIFT_HLSLI

static const float3 kLiftLuma = float3(0.2126, 0.7152, 0.0722);

// The lift's gain at a pixel whose block mean is small, whose surroundings' mean is around, and whose lens CoC is coc
// render pixels on a picture rows tall.
float lift_gain(float small, float around, float coc, float rows)
{
    float ratio = small / max(around, 1e-4);
    float gain = clamp(1 + accum_lift * max(ratio - accum_lift_threshold, 0), 1, 1 + 2 * accum_lift);
    float fade = saturate((coc - rows / 400) / (rows * 3 / 400));
    return 1 + (gain - 1) * fade;
}

#endif
