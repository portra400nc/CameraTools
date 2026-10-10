#include "accumulate.h"

#include <algorithm>
#include <cmath>

namespace photoreal
{
    namespace
    {
    namespace hlsl
    {
        struct float2
        {
            float x, y;
            float2(float x_, float y_) : x(x_), y(y_) {}
        };
        float2 operator-(float2 a, float2 b) { return { a.x - b.x, a.y - b.y }; }
        float2 operator*(float2 a, float s) { return { a.x * s, a.y * s }; }
        float length(float2 v) { return std::sqrt(v.x * v.x + v.y * v.y); }
        float sqrt(float x) { return std::sqrt(x); }
        float smoothstep(float edge0, float edge1, float x)
        {
            const float t = std::fmin(std::fmax((x - edge0) / (edge1 - edge0), 0.0f), 1.0f);
            return t * t * (3 - 2 * t);
        }

#include "../shaders/cat_eye.hlsli"
    }
    }

    AccumulateTick accumulate_tick(const Accumulator &state, const AccumulateSettings &settings, const LensSample &lens, bool camera_fresh,
        Size render)
    {
        AccumulateTick tick;
        tick.next = state;
        const bool stale = state.generation != settings.generation || state.render != render;
        switch (settings.mode)
        {
        case AccumulateMode::add:
            if (stale)
            {
                tick.clear = true;
                tick.next = { settings.generation, render, 0, std::nullopt };
            }
            tick.add = lens.mark && camera_fresh && tick.next.last_index != lens.index;
            if (tick.add)
            {
                tick.next.samples++;
                tick.next.last_index = lens.index;
            }
            break;
        case AccumulateMode::present:
            tick.present = !stale && state.samples > 0;
            break;
        case AccumulateMode::off:
        case AccumulateMode::count:
            break;
        }
        return tick;
    }

    float cat_eye_weight(float lens_x, float lens_y, float ndc_x, float ndc_y, float aspect, float cat_eye, float falloff)
    {
        return hlsl::cat_eye_weight({ lens_x, lens_y }, { ndc_x, ndc_y }, aspect, cat_eye, falloff);
    }

    std::optional<Pixel> depth_pixel(Size render, float u, float v, bool flip)
    {
        if (!(u >= 0 && u <= 1 && v >= 0 && v <= 1) || render.width == 0 || render.height == 0)
            return std::nullopt;
        const float row = flip ? 1 - v : v;
        return Pixel { std::min(static_cast<uint32_t>(u * render.width), render.width - 1),
            std::min(static_cast<uint32_t>(row * render.height), render.height - 1) };
    }

    std::optional<float> view_depth(const Camera &camera, float u, float v, float reversed_depth)
    {
        float clip_to_view[16];
        if (!invert(camera.view_to_clip, clip_to_view))
            return std::nullopt;
        const float clip[4] = { u * 2 - 1, 1 - v * 2, reversed_depth, 1 };
        float view[4] = {};
        for (int row = 0; row < 4; row++)
            for (int column = 0; column < 4; column++)
                view[row] += clip_to_view[column * 4 + row] * clip[column];
        if (view[3] == 0)
            return std::nullopt;
        const float depth = -view[2] / view[3];
        if (!std::isfinite(depth) || depth <= 0)
            return std::nullopt;
        return depth;
    }
}
