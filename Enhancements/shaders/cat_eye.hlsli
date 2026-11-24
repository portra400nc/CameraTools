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

// How much a lens sample counts at a pixel under swirl, as the depth of field's gather squeezes a disc (dof.hlsli,
// dof_shape_distance): the lens point is stretched along the line from the picture's center by 1.5 swirl times the
// square of the pixel's distance from it, with the corners at 1, and counts while it stays within 1, its edge soft over
// falloff. So toward the corners the aperture narrows along that line and bokeh turns to ovals around the center.
float swirl_weight(float2 lens, float2 ndc, float aspect, float swirl, float falloff)
{
    float2 at = float2(ndc.x * aspect, ndc.y) / sqrt(aspect * aspect + 1);
    float m = length(at);
    if (swirl <= 0 || m < 0.0001)
        return 1;
    float2 e = at / m;
    float2 u = lens + e * (dot(lens, e) * 1.5 * swirl * m * m);
    return 1 - smoothstep(1 - falloff, 1, length(u));
}

// Where a lens sample's red or blue is read, in the picture's uv from the top left: chromatic render pixels per unit of
// the lens point further along it than the pixel, y up on the lens and down in uv. The pixel's own place for 0.
float2 chromatic_uv(float2 uv, float2 lens, float chromatic, float2 texel)
{
    return uv + float2(lens.x * texel.x, -lens.y * texel.y) * chromatic;
}
