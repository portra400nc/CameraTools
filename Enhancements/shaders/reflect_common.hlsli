// What the reflection kernel and its apply draw agree on. keep is 1 - gloss. A gloss of 0 keeps the stored roughness, and of 1
// mirrors. Between them roughness has a floor, so a floor the rain made smooth still blurs its bumps a little: 0.12 at a gloss of 0.5.
#ifndef REFLECT_COMMON_HLSLI
#define REFLECT_COMMON_HLSLI

float reflect_roughness_of(float smoothness, float keep)
{
    return max((1.0f - smoothness) * keep, 0.48f * (1.0f - keep) * keep);
}

#endif
