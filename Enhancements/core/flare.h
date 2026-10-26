// The lens flare: its settings, its constants as the shaders read them, where the sun falls on the picture, and
// shaders/flare.hlsli compiled as C++ (core/flare.cpp), so the tests and the Mac's harness draw the shader's own flare.
// Pure C++17 like plan.h. The GPU side is in gpu.cpp.
#pragma once

#include "plan.h"

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace enhancements
{
    // shaders/flare.cbuffer.hlsli's cbuffer Flare at b1: 80 bytes.
    struct FlareConstants
    {
        float sun[4];       // the source in picture units, its visibility times the strength, width over height
        float amounts[4];   // arcs, ghosts, streaks, glare
        float tint[4];      // rgb linear, w size
        float frame[4];     // back buffer width and height, 1 when sRGB-encoded, chromatic spread
        float more[4];      // filled arcs, 1 when the target's rows run up the screen (a game target stored upside down); two unused
    };
    static_assert(sizeof(FlareConstants) == 80, "the cbuffer's size");

    // Where the sun falls on the picture: in picture units (height 1, center 0, x right, y down), and whether it lies in
    // front of the camera. Off the picture, even far off, the position is still the projection's.
    struct FlareSun
    {
        float x = 0, y = 0;
        bool in_front = false;
    };

    // A sun whose direction's height (world y) is below this has set.
    constexpr float kFlareHorizon = -0.03f;

    // The sky's sun disc through the camera's view and projection, upright whatever the flip. Empty without a sun, or with one
    // that has set.
    std::optional<FlareSun> flare_sun(const Camera &camera);

    // A point from 0 to 1 from the top left in picture units, on a picture of aspect width over height.
    FlareSun flare_point(float u, float v, float aspect);

    // The radius of the disc around the sun on the picture whose depth says how much of the sun shows, over the height: about
    // twice the real sun's at the game's usual field of view, so a leaf's edge dims it smoothly.
    constexpr float kFlareDisc = 0.01f;

    // The share of the way the visibility moves toward this frame's in a frame of frame_seconds, so it covers 99% of it in
    // seconds; 1 at 0 seconds.
    float flare_ease(float seconds, float frame_seconds);

    // The constants for a target of size, the source at sun, whether the target is sRGB-encoded, and whether its rows run up
    // the screen, as the game's own targets do while EnhancementsSettings.flip is on.
    FlareConstants flare_constants(const FlareSettings &settings, const FlareSun &sun, Size back, bool srgb, bool flip = false);

    // flare_light at every pixel center over a picture of the constants' size, screen-blended over linear, three floats a
    // pixel, upright; visibility is what flare_visibility_ps.hlsl's texture holds.
    std::vector<float> render_flare(const std::vector<float> &linear, const FlareConstants &constants, float visibility);

    // flare_light alone at one pixel center.
    std::array<float, 3> flare_light_at(float x, float y, const FlareConstants &constants, float visibility);
}
