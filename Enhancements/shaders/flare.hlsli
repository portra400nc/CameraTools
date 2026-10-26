// The lens flare: light a bright source just inside or outside the frame scatters and reflects inside an old, single-coated
// lens, drawn over the finished picture. Four parts, after the vintage portraits the look follows:
//   arcs     thin orange-red rings near the source, partly drawn, from light bouncing off the barrel's inner edges
//   ghosts   soft discs with a bright spot, on the line from the source through the picture's center, from reflections
//            between the elements
//   streaks  thin lines along that line, from light glancing off the glass
//   glare    a warm haze toward the source, from light scattered inside the lens
// Positions are in picture units: the height is 1, the center 0, x right and y down. Shared by flare_ps.hlsl and,
// compiled as C++, core/flare.cpp. Include flare.cbuffer.hlsli first.
//
// The includer defines, before the include:
//   float flare_load_visibility()   the sun's visibility, 0 to 1, from flare_visibility_ps.hlsl's 1x1 texture

#define FLARE_RINGS 4
#define FLARE_GHOSTS 6
#define FLARE_STREAKS 3

// Each ring: (where its center lies along the line, as a multiple of the source's position; radius; line width; light)
// and (the turn of its drawn part from the side facing the picture's center, in radians; how narrow that part is; light
// filling its inside; how far out its red fringe lies, over the radius).
static const float4 kFlareRingA[FLARE_RINGS] = {
    float4(1.00f, 0.17f, 0.0050f, 0.80f),
    float4(1.00f, 0.30f, 0.0040f, 0.25f),
    float4(0.30f, 0.55f, 0.0050f, 0.20f),
    float4(-0.30f, 0.95f, 0.0060f, 0.18f),
};
static const float4 kFlareRingB[FLARE_RINGS] = {
    float4(-0.70f, 3.0f, 0.06f, 0.015f),
    float4(-1.20f, 6.0f, 0.04f, 0.012f),
    float4(0.20f, 6.0f, 0.02f, 0.010f),
    float4(2.60f, 8.0f, 0.00f, 0.008f),
};

// Each ghost: (where its center lies along the line; radius; light; its bright spot's light) and (color; edge softness).
static const float4 kFlareGhostA[FLARE_GHOSTS] = {
    float4(-1.16f, 0.150f, 0.45f, 3.0f),
    float4(-0.60f, 0.080f, 0.20f, 0.8f),
    float4(-1.80f, 0.320f, 0.10f, 0.0f),
    float4(0.74f, 0.012f, 0.60f, 0.0f),
    float4(0.45f, 0.040f, 0.12f, 0.0f),
    float4(-0.35f, 0.025f, 0.15f, 0.0f),
};
static const float4 kFlareGhostB[FLARE_GHOSTS] = {
    float4(1.00f, 0.40f, 0.12f, 0.70f),
    float4(1.00f, 0.28f, 0.10f, 0.60f),
    float4(1.00f, 0.25f, 0.08f, 0.80f),
    float4(0.60f, 0.90f, 1.00f, 0.40f),
    float4(1.00f, 0.55f, 0.25f, 0.50f),
    float4(0.40f, 1.00f, 0.50f, 0.50f),
};

// Each streak: (its offset across the line; width; light; bend) and (where along the line it starts and ends, relative to
// the source's distance from the center; unused; unused).
static const float4 kFlareStreakA[FLARE_STREAKS] = {
    float4(0.000f, 0.016f, 0.20f, 0.02f),
    float4(0.050f, 0.010f, 0.15f, 0.02f),
    float4(-0.090f, 0.012f, 0.10f, 0.01f),
};
static const float4 kFlareStreakB[FLARE_STREAKS] = {
    float4(-0.3f, 1.00f, 0.0f, 0.0f),
    float4(-0.1f, 0.95f, 0.0f, 0.0f),
    float4(0.1f, 0.90f, 0.0f, 0.0f),
};

float2 flare_normalize(float2 v, float2 fallback)
{
    float l = length(v);
    return l > 1e-5f ? v / l : fallback;
}

float2 flare_rotate(float2 v, float angle)
{
    float c = cos(angle), s = sin(angle);
    return float2(v.x * c - v.y * s, v.x * s + v.y * c);
}

// How far the source lies outside the picture, in picture units: negative inside, 0 on its edge.
float flare_outside(float2 sun)
{
    return max(abs(sun.x) - flare_sun.w * 0.5f, abs(sun.y) - 0.5f);
}

// A ring's light at a distance d from its line, signed (negative inside): a thin orange line, a red fringe just outside
// it, and a fill that is brightest just inside the line and fades toward the center.
float3 flare_ring_light(float d, float width, float fill, float radius, float fringe)
{
    float stroke = exp(-(d * d) / (width * width));
    float e = d - fringe * radius;
    float halo = 0.6f * exp(-(e * e) / (9.0f * width * width));
    float inside = d < 0.0f ? fill * exp(d / (0.35f * radius)) : 0.0f;
    return float3(1.0f, 0.45f, 0.18f) * stroke + float3(1.0f, 0.2f, 0.07f) * halo + float3(1.0f, 0.55f, 0.3f) * inside;
}

float3 flare_rings(float2 q, float2 sun, float2 axis, float size)
{
    float3 sum = float3(0.0f, 0.0f, 0.0f);
    UNROLL for (int i = 0; i < FLARE_RINGS; i++)
    {
        float4 a = kFlareRingA[i];
        float4 b = kFlareRingB[i];
        float2 center = sun * a.x;
        float2 offset = q - center;
        float2 facing = flare_normalize(-center, -axis);
        float2 drawn = flare_rotate(facing, b.x);
        float part = pow(saturate(0.5f + 0.5f * dot(flare_normalize(offset, drawn), drawn)), b.y);
        float radius = a.y * size;
        sum = sum + flare_ring_light(length(offset) - radius, a.z * size, b.z, radius, b.w * flare_frame.w) * (a.w * part);
    }
    return sum;
}

// A ghost's light in one channel at t, the distance from its center over its radius: a disc brighter toward its rim, with
// a soft edge and a thin bright rim.
float flare_ghost_profile(float t, float softness)
{
    float disc = 1.0f - smoothstep(1.0f - softness, 1.0f, t);
    float rim = 0.15f * exp(-((t - 1.0f) * (t - 1.0f)) / (0.0036f + 0.02f * softness));
    return disc * (0.55f + 0.45f * t * t) + rim;
}

float3 flare_ghosts(float2 q, float2 sun, float2 axis, float size)
{
    float3 sum = float3(0.0f, 0.0f, 0.0f);
    UNROLL for (int i = 0; i < FLARE_GHOSTS; i++)
    {
        float4 a = kFlareGhostA[i];
        float4 b = kFlareGhostB[i];
        float2 center = sun * a.x;
        float radius = a.y * size;
        float r = length(q - center);
        float spread = 0.02f * flare_frame.w;
        float red = flare_ghost_profile(r / (radius * (1.0f + spread)), b.w);
        float green = flare_ghost_profile(r / radius, b.w);
        float blue = flare_ghost_profile(r / (radius * (1.0f - spread)), b.w);
        // The bright spot sits toward the ghost's side away from the picture's center.
        float2 outward = flare_normalize(center, a.x < 0.0f ? -axis : axis);
        float2 spot = center + outward * (0.55f * radius);
        // Drawn out along the line through the center, like a flame.
        float2 to_spot = q - spot;
        float along = dot(to_spot, outward), across = dot(to_spot, float2(-outward.y, outward.x));
        float spot_light = a.w * exp(-(along * along / 0.09f + across * across / 0.02f) / (radius * radius));
        sum = sum + float3(b.x * red, b.y * green, b.z * blue) * a.z + float3(1.0f, 0.5f, 0.2f) * spot_light * a.z;
    }
    return sum;
}

float3 flare_streaks(float2 q, float2 sun, float2 axis, float size)
{
    float distance = max(length(sun), 0.25f);
    float2 across = float2(-axis.y, axis.x);
    float sum = 0.0f;
    UNROLL for (int i = 0; i < FLARE_STREAKS; i++)
    {
        float4 a = kFlareStreakA[i];
        float4 b = kFlareStreakB[i];
        float along = dot(q, axis) / distance;
        float start = b.x, end = b.y;
        float middle = 0.5f * (start + end);
        float d = dot(q, across) - a.x * size - a.w * (along - middle) * (along - middle);
        float width = a.y * size;
        float stroke = exp(-(d * d) / (width * width)) + 0.15f * exp(-(d * d) / (0.02f * width * width));
        float fade = 0.25f * (end - start);
        float taper = smoothstep(start, start + fade, along) * (1.0f - smoothstep(end - fade, end, along));
        sum += stroke * taper * a.z;
    }
    // Light glances off the glass mostly while the source lies at or past the picture's edge.
    float glancing = 0.1f + 0.9f * smoothstep(-0.15f, 0.1f, flare_outside(sun));
    return float3(1.0f, 0.48f, 0.18f) * (sum * glancing);
}

float3 flare_glare(float2 q, float2 sun)
{
    float2 d = q - sun;
    float r2 = dot(d, d);
    float haze = 0.30f / (1.0f + r2 / 0.02f) + 0.10f * exp(-r2 / 0.3f) + 0.02f;
    return float3(1.0f, 0.62f, 0.34f) * haze;
}

// The flare's light at a pixel center, linear, before the screen blend: rgb.
float3 flare_light(float2 pixel)
{
    float aspect = flare_sun.w;
    float2 q = float2((pixel.x / flare_frame.x - 0.5f) * aspect, pixel.y / flare_frame.y - 0.5f);
    float2 sun = float2(flare_sun.x, flare_sun.y);
    float visibility = flare_sun.z * flare_load_visibility();
    if (visibility <= 0.0f)
        return float3(0.0f, 0.0f, 0.0f);
    float2 axis = flare_normalize(sun, float2(0.0f, -1.0f));
    // Every part fades as the source leaves the picture, and is gone a little over half the height beyond its edge.
    float outside = flare_outside(sun);
    float present = 1.0f - smoothstep(0.1f, 0.65f, outside);
    float size = flare_tint.w;
    float3 light = flare_rings(q, sun, axis, size) * flare_amounts.x + flare_ghosts(q, sun, axis, size) * flare_amounts.y
        + flare_streaks(q, sun, axis, size) * flare_amounts.z + flare_glare(q, sun) * flare_amounts.w;
    float3 tint = float3(flare_tint.x, flare_tint.y, flare_tint.z);
    return light * tint * (visibility * present);
}

// The picture's linear color under the flare: a screen blend, which, like light added on film, never passes white.
float3 flare_over(float3 color, float3 light)
{
    return float3(1.0f - (1.0f - saturate(color.x)) * (1.0f - saturate(light.x)), 1.0f - (1.0f - saturate(color.y)) * (1.0f - saturate(light.y)),
        1.0f - (1.0f - saturate(color.z)) * (1.0f - saturate(light.z)));
}
