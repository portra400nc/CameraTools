// The highlight lift of lens-sampled shots. The game's HDR scene is shallow: a lamp, a lantern or a gold ornament stands
// only a few times above what is around it, where a real one, much brighter, keeps its disc when blurred. Each sample's
// out-of-focus light is multiplied by how far it stands above its own surroundings before its gap fill and the sum: by
// 1 + 4 strength (ratio - t), at most 1 + 2 strength, where ratio is the mean of a block, a 240th of the picture's height
// a side, over the brightest block on a ring four blocks out: a bright strip, as a sunlit wall's top, or a bright
// area's edge or corner have bright blocks on their ring and take none (the mean of the blocks around lifted those into
// a haze over what lay next to them). Each block then takes the largest gain of the 3 x 3 blocks around it, read
// bilinear, so a whole ornament takes its brightest part's gain: blocks its own size lifted part of it and left crosses
// in its disc. A pixel takes that gain only as far as it stands above the brightest of the surroundings it was measured
// against, fully at twice it: the dark, far background beside an ornament near focus, which the spread gain reached,
// kept a bright ring around it. t is 1 + (threshold - 1) / 4: the row's 2 is 1.25.
// The gain fades in with the lens's CoC from a quarter of a 400-row picture's pixel to one, so what is in focus stays
// as the game drew it, and a wider disc spreads the lifted light thinner, as a real lens's does.
// (.work/dof/lift/render_frame.py tried it on a saved frame against an exposure lift and an expansion curve.)
#ifndef LIFT_HLSLI
#define LIFT_HLSLI

static const float3 kLiftLuma = float3(0.2126, 0.7152, 0.0722);

// The gain of a block whose mean is small and the brightest block on whose ring is brightest.
float lift_block_gain(float small, float brightest)
{
    float ratio = small / max(brightest, 1e-4);
    float t = 1 + (accum_lift_threshold - 1) / 4;
    return clamp(1 + 4 * accum_lift * max(ratio - t, 0), 1, 1 + 2 * accum_lift);
}

// The gain at a pixel of luminance l whose blocks' gain and surroundings' brightest are spread, and whose lens CoC is coc
// render pixels on a picture rows tall.
float lift_faded(float l, float2 spread, float coc, float rows)
{
    float own = saturate(l / max(spread.y, 1e-4) - 1);
    return 1 + (spread.x - 1) * own * saturate((coc - rows / 400) / (rows * 3 / 400));
}

#endif
