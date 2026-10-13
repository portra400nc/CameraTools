// The cat's-eye weight, shared by shaders/accumulate_ps.hlsl and core/accumulate.cpp, which compiles it as C++ so the
// native tests check the shader's own arithmetic. Keep it to what both languages read alike.

// How much a lens sample counts at a pixel. Seen from off the axis, a real lens's aperture is cut by a second disk of
// the same size whose center moves out with the pixel: only lens points within 1 of that center pass, and the edge is
// soft over falloff. ndc runs from -1 to 1 with y up; aspect is width over height. The center is ndc stretched to the
// picture's shape and scaled so the corners lie at distance 1, times cat_eye.
float cat_eye_weight(float2 lens, float2 ndc, float aspect, float cat_eye, float falloff)
{
    float2 center = float2(ndc.x * aspect, ndc.y) * (cat_eye / sqrt(aspect * aspect + 1));
    return 1 - smoothstep(1 - falloff, 1, length(lens - center));
}
