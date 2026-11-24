// The highlight lift of lens-sampled shots. The game's HDR scene is shallow: a lamp or a glint of sun lies only a few
// times above what is around it, so averaged over a lens's disc it all but vanishes, where a real lamp's light, many
// times brighter, keeps its disc. Each sample is expanded before its gap fill and the samples' sum, and the average is
// compressed back by the exact inverse, so what every sample shows alike, the picture in focus, comes back as it was,
// and what spreads keeps the share of light a brighter scene would give it.
//
// By luminance, so hues stay: x = L / t goes to g(x) = min(1 + (exp(s (x - 1)) - 1) / s, kLiftMostGain x) above 1, which
// meets t with slope 1. The cap keeps a sun's texel within the scene's float format; both parts rise, so the inverse is
// the larger of their inverses. t is the threshold in the scene's units, s the strength (0: off).
#ifndef LIFT_HLSLI
#define LIFT_HLSLI

static const float kLiftMostGain = 256.0;
static const float3 kLiftLuma = float3(0.2126, 0.7152, 0.0722);

float3 lift_expand(float3 c, float t, float s)
{
    float l = dot(c, kLiftLuma);
    float x = l / max(t, 1e-6);
    if (s <= 0 || x <= 1)
        return c;
    float g = min(1 + (exp(min(s * (x - 1), 80.0)) - 1) / s, kLiftMostGain * x);
    return c * (g / x);
}

float3 lift_compress(float3 c, float t, float s)
{
    float l = dot(c, kLiftLuma);
    float y = l / max(t, 1e-6);
    if (s <= 0 || y <= 1)
        return c;
    float x = max(1 + log(1 + s * (y - 1)) / s, y / kLiftMostGain);
    return c * (x / y);
}

#endif
