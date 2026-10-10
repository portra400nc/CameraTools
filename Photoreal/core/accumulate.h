// The accumulator's decisions: which frames go into the sum, when it empties, the cat's-eye weight, and where the depth
// read looks and what it means. Pure C++17 like plan.h. The shell draws and reads back; this says when and what.
#pragma once

#include "plan.h"

#include <optional>

namespace photoreal
{
    // What the sum holds, on the render thread.
    struct Accumulator
    {
        uint32_t generation = 0;
        Size render;                        // a sum is only good at the size it was made at
        uint32_t samples = 0;
        std::optional<uint32_t> last_index; // the newest sample added, so a sample marked on two frames adds once
    };

    // What the shell does at one bloom step, in field order, and the sum's state once it did.
    struct AccumulateTick
    {
        bool clear = false;     // empty the sum
        bool add = false;       // add this frame's HDR scene
        bool present = false;   // write the sum's average over the HDR scene
        Accumulator next;
    };

    // camera_fresh: a camera push arrived for this frame, so its lens sample is this frame's. The sum empties when the
    // generation or the render size changed, in add mode only. A frame adds when its lens sample is marked, its camera is
    // fresh, and it is not the sample added last. Present mode presents a sum of this generation and size with at least
    // one frame in it.
    AccumulateTick accumulate_tick(const Accumulator &state, const AccumulateSettings &settings, const LensSample &lens, bool camera_fresh,
        Size render);

    // shaders/cat_eye.hlsli's weight, compiled from the same source.
    float cat_eye_weight(float lens_x, float lens_y, float ndc_x, float ndc_y, float aspect, float cat_eye, float falloff);

    struct Pixel
    {
        uint32_t x = 0, y = 0;
        friend bool operator==(Pixel a, Pixel b) { return a.x == b.x && a.y == b.y; }
    };

    // The stored pixel of a screen point, u and v from 0 at the top left to 1. Game targets are stored upside down when
    // flip is set. Empty when the point is off the screen or the size is empty.
    std::optional<Pixel> depth_pixel(Size render, float u, float v, bool flip);

    // The view-space depth in meters, along the camera's forward axis, of a screen point whose reversed depth buffer holds
    // reversed_depth, through the inverse of camera.view_to_clip. Empty when the projection does not invert or the
    // point lies behind the camera.
    std::optional<float> view_depth(const Camera &camera, float u, float v, float reversed_depth);
}
