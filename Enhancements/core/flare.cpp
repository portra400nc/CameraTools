// shaders/flare.hlsli compiled as C++ through hlsl.h. Its constants are this thread's copies of the shaders' cbuffer Flare.
#include "flare.h"
#include "hlsl.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace enhancements::shader
{
    thread_local float4 flare_sun, flare_amounts, flare_tint, flare_frame, flare_more;
    thread_local float flare_visibility = 1;

    float flare_load_visibility() { return flare_visibility; }

#define LOOP
#define UNROLL
#include "../shaders/flare.hlsli"
#undef LOOP
#undef UNROLL
}

namespace enhancements
{
    namespace
    {
        float lerp(float a, float b, float t) { return a + (b - a) * t; }

        void load(const FlareConstants &c, float visibility)
        {
            using shader::float4;
            shader::flare_sun = float4(c.sun[0], c.sun[1], c.sun[2], c.sun[3]);
            shader::flare_amounts = float4(c.amounts[0], c.amounts[1], c.amounts[2], c.amounts[3]);
            shader::flare_tint = float4(c.tint[0], c.tint[1], c.tint[2], c.tint[3]);
            shader::flare_frame = float4(c.frame[0], c.frame[1], c.frame[2], c.frame[3]);
            shader::flare_more = float4(c.more[0], c.more[1], c.more[2], c.more[3]);
            shader::flare_visibility = visibility;
        }
    }

    std::optional<FlareSun> flare_sun(const Camera &camera)
    {
        const float *d = camera.toward_sky_sun;
        // Below the horizon it has set: the night's light is the moon's.
        if ((d[0] == 0 && d[1] == 0 && d[2] == 0) || d[1] < kFlareHorizon)
            return std::nullopt;
        // Column-major: element (row r, column c) at [c * 4 + r]. A direction has w 0.
        const float *v = camera.world_to_view, *p = camera.view_to_clip;
        float view[3];
        for (int r = 0; r < 3; r++)
            view[r] = v[r] * d[0] + v[4 + r] * d[1] + v[8 + r] * d[2];
        float clip[4];
        for (int r = 0; r < 4; r++)
            clip[r] = p[r] * view[0] + p[4 + r] * view[1] + p[8 + r] * view[2];
        FlareSun sun;
        sun.in_front = clip[3] > 1e-6f;
        const float w = sun.in_front ? clip[3] : 1e-6f;
        const float aspect = p[5] != 0 && p[0] != 0 ? p[5] / p[0] : 1.6f;
        // Clip y runs up; the picture's y runs down.
        sun.x = clip[0] / w * 0.5f * aspect;
        sun.y = -clip[1] / w * 0.5f;
        return sun;
    }

    float flare_ease(float seconds, float frame_seconds)
    {
        return seconds > 0 ? 1.0f - std::pow(0.01f, std::max(frame_seconds, 0.0f) / seconds) : 1.0f;
    }

    FlareSun flare_point(float u, float v, float aspect) { return { (u - 0.5f) * aspect, v - 0.5f, true }; }

    FlareConstants flare_constants(const FlareSettings &settings, const FlareSun &sun, Size back, bool srgb, bool flip)
    {
        FlareConstants c {};
        const float aspect = back.height > 0 ? float(back.width) / float(back.height) : 1.6f;
        // Far outside the picture the projection grows without bound; the flare has long faded by then.
        c.sun[0] = std::clamp(sun.x, -100.0f, 100.0f);
        c.sun[1] = std::clamp(sun.y, -100.0f, 100.0f);
        c.sun[2] = sun.in_front ? settings.strength : 0.0f;
        c.sun[3] = aspect;
        c.amounts[0] = settings.arcs;
        c.amounts[1] = settings.ghosts;
        c.amounts[2] = settings.streaks;
        c.amounts[3] = settings.glare;
        // White at 0.5, the elements' own colors; toward 0 cooler, toward 1 deeper orange.
        const float w = settings.warmth;
        c.tint[0] = w < 0.5f ? lerp(0.78f, 1.0f, w * 2) : 1.0f;
        c.tint[1] = w < 0.5f ? lerp(0.95f, 1.0f, w * 2) : lerp(1.0f, 0.72f, w * 2 - 1);
        c.tint[2] = w < 0.5f ? lerp(1.25f, 1.0f, w * 2) : lerp(1.0f, 0.5f, w * 2 - 1);
        c.tint[3] = settings.size;
        c.frame[0] = float(back.width);
        c.frame[1] = float(back.height);
        c.frame[2] = srgb ? 1.0f : 0.0f;
        c.frame[3] = settings.chromatic;
        c.more[0] = settings.filled_arcs;
        c.more[1] = flip ? 1.0f : 0.0f;
        c.more[2] = settings.rainbow;
        return c;
    }

    std::array<float, 3> flare_light_at(float x, float y, const FlareConstants &constants, float visibility)
    {
        load(constants, visibility);
        const shader::float3 l = shader::flare_light(shader::float2(x, y));
        return { l.x, l.y, l.z };
    }

    std::vector<float> render_flare(const std::vector<float> &linear, const FlareConstants &constants, float visibility)
    {
        load(constants, visibility);
        const uint32_t w = uint32_t(constants.frame[0]), h = uint32_t(constants.frame[1]);
        std::vector<float> out(size_t(w) * h * 3);
        for (uint32_t y = 0; y < h; y++)
            for (uint32_t x = 0; x < w; x++)
            {
                const size_t at = (size_t(y) * w + x) * 3;
                const shader::float3 light = shader::flare_light(shader::float2(float(x) + 0.5f, float(y) + 0.5f));
                const shader::float3 c = shader::flare_over(shader::float3(linear[at], linear[at + 1], linear[at + 2]), light);
                out[at] = c.x;
                out[at + 1] = c.y;
                out[at + 2] = c.z;
            }
        return out;
    }
}
