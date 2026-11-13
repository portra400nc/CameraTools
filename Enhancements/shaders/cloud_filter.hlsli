// Catmull-Rom filtering in nine bilinear taps (after Jimenez's five-tap form, corners kept): sharper than bilinear, so the
// cloud history does not blur a little more each frame it is moved, and the upscale keeps the clouds' edges.
float4 sample_catmull_rom(Texture2D<float4> t, float2 uv, float2 size)
{
    float2 at = uv * size;
    float2 center = floor(at - 0.5) + 0.5;
    float2 f = at - center;
    float2 w0 = f * (-0.5 + f * (1 - 0.5 * f));
    float2 w1 = 1 + f * f * (-2.5 + 1.5 * f);
    float2 w2 = f * (0.5 + f * (2 - 1.5 * f));
    float2 w3 = f * f * (-0.5 + 0.5 * f);
    float2 w12 = w1 + w2;
    float2 offset12 = w2 / w12;
    float2 p0 = (center - 1) / size, p3 = (center + 2) / size, p12 = (center + offset12) / size;
    float4 sum = 0;
    sum += t.SampleLevel(linear_clamp, float2(p0.x, p0.y), 0) * w0.x * w0.y;
    sum += t.SampleLevel(linear_clamp, float2(p12.x, p0.y), 0) * w12.x * w0.y;
    sum += t.SampleLevel(linear_clamp, float2(p3.x, p0.y), 0) * w3.x * w0.y;
    sum += t.SampleLevel(linear_clamp, float2(p0.x, p12.y), 0) * w0.x * w12.y;
    sum += t.SampleLevel(linear_clamp, float2(p12.x, p12.y), 0) * w12.x * w12.y;
    sum += t.SampleLevel(linear_clamp, float2(p3.x, p12.y), 0) * w3.x * w12.y;
    sum += t.SampleLevel(linear_clamp, float2(p0.x, p3.y), 0) * w0.x * w3.y;
    sum += t.SampleLevel(linear_clamp, float2(p12.x, p3.y), 0) * w12.x * w3.y;
    sum += t.SampleLevel(linear_clamp, float2(p3.x, p3.y), 0) * w3.x * w3.y;
    return sum;
}
