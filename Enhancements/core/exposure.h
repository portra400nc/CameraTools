// Auto exposure's metering and easing: shaders/exposure.hlsli compiled as C++ (core/exposure.cpp), so the tests check the
// shaders' own math.
#pragma once

namespace enhancements
{
    // The auto exposure covers 99% of the way to a new scene's exposure in this many seconds.
    constexpr float kAutoExposureSeconds = 1.0f;

    // A pixel's log2 luminance as the meter takes it.
    float exposure_log_luminance(float r, float g, float b);

    // The stops the metering adds to the game's exposure for a scene whose mean log2 luminance is mean_log2.
    float exposure_target_ev(float mean_log2, float game_exposure, float strength, float night_ev = 0);

    // This frame's stops, eased from previous toward target by ease.
    float exposure_next_ev(float previous, float target, float ease);
}
