// The wetness pass's arithmetic, shared by shaders/wetness_ps.hlsl and core/plan.cpp, which compiles it as C++ so the
// native tests check the shader's own formulas. Keep it to what both languages read alike.

// How much puddle covers a pixel whose world normal's y is up, where the world's puddle noise is noise: on near-flat
// ground (up above 0.95) the noise rises above a threshold that falls from 1 as wetness times puddles grows, with soft
// edges in the noise and in the slope.
float puddle_cover(float noise, float up, float wetness, float puddles)
{
    float threshold = 1 - puddles * wetness;
    return smoothstep(threshold, threshold + 0.05, noise) * smoothstep(0.95, 0.98, up);
}

// How wet a pixel is: wetness on the ground, half of it on walls and overhangs, and all the way to 1 in a puddle.
float wet_share(float wetness, float up, float puddle) { return lerp(wetness * (0.5 + 0.5 * saturate(up)), 1, puddle); }

// Smoothness rises to 0.9 as the surface wets, and to 0.97 in a puddle.
float wet_smoothness(float smoothness, float wet, float puddle) { return lerp(lerp(smoothness, 0.9, wet), 0.97, puddle); }
