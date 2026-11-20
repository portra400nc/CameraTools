// Shared by the rain shaders, after common.hlsli: the hashes, the noise and the gusts that thicken and thin the splashes
// and the drops alike, and the drops' streak from their vertex shader to their pixel shader.

static const float kShutter = 1.0 / 25;     // the seconds a streak shows of a drop's fall, a film camera's blur

float2 hash2(float2 p)
{
    p = frac(p * float2(0.1031, 0.1030));
    p += dot(p, p.yx + 33.33);
    return frac((p.xx + p.yx) * p.yx);
}

float noise1(float x)
{
    float i = floor(x), f = frac(x);
    return lerp(hash2(float2(i, 3.7)).x, hash2(float2(i + 1, 3.7)).x, f * f * (3 - 2 * f));
}

float noise2(float2 p)
{
    float2 i = floor(p), f = frac(p);
    f = f * f * (3 - 2 * f);
    return lerp(lerp(hash2(i).x, hash2(i + float2(1, 0)).x, f.x), lerp(hash2(i + float2(0, 1)).x, hash2(i + 1).x, f.x), f.y);
}

// Gusts: the rain thickens and thins in sheets drifting around the camera, azimuth radians from world +x toward +z.
float rain_gust(float azimuth)
{
    return lerp(0.45, 1.35, noise2(float2(azimuth * 1.6 - rain_time * 0.12, rain_time * 0.2)));
}

// A drop's streak, from rain_drops_vs.hlsl to rain_drops_ps.hlsl. Its quad's pixels are those of the stored target.
struct RainStreak
{
    float4 position : SV_Position;
    noperspective float2 at : TEXCOORD0;        // pixels along the streak from its head, and across it from its middle line
    noperspective float inverse_meters : TEXCOORD1; // 1 over the view depth, which interpolates linearly on the screen
    nointerpolation float4 shape : TEXCOORD2;   // its length and drawn width in pixels, and the unit direction across it
    nointerpolation float4 light : TEXCOORD3;   // rgb its own light; a the share of a pixel it takes across its drawn width
};
