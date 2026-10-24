// The depth of field's arithmetic: shaders/dof.hlsli compiled as C++ (core/dof.cpp), so the tests check the shaders'
// own circles of confusion, aperture shape and light across the disc, and the shape's constants the shaders take from
// the CPU. Pure C++17 like plan.h.
#pragma once

#include <array>
#include <cstdint>

namespace enhancements
{
    // The lens as dof.hlsli's dof_lens_at takes it.
    struct DofOptics
    {
        float focal_mm = 50;
        float f_number = 2;
        float pixels_per_mm = 1;    // half-size pixels per millimeter of sensor: render height / 2 / sensor height
        float chromatic = 0;
        float max_radius = 1e9f;    // half-size pixels
    };

    // The aperture as dof.hlsli's DofShape holds it, before the area scale.
    struct DofShapeParams
    {
        uint32_t blades = 0;        // 0 round, else 3 or more
        float roundness = 0;
        float rotation_degrees = 0;
        float anamorphic = 1;
        float cat_eye = 0;
        float swirl = 0;
        float aspect = 1.6f;
    };

    // The signed CoC radius of each channel, red, green and blue, in half-size pixels, of a point depth meters away with
    // the lens focused at focus meters, through dof_channel_coc from green's as the passes take it. Not clamped.
    std::array<float, 3> dof_channel_cocs(float depth, float focus, const DofOptics &optics);

    // The green CoC straight from dof_coc.
    float dof_green_coc(float depth, float focus, const DofOptics &optics);

    // How much of a source of radius r half-size pixels at ndc (y up) reaches a receiver u pixels from its center (y up):
    // dof_aperture, with the shape's area scale.
    float dof_aperture(float ux, float uy, float r, float ndc_x, float ndc_y, const DofShapeParams &shape);

    // dof_bokeh_intensity at rho with aberration (the setting's behind focus, its negative in front) and its
    // normalization.
    float dof_bokeh_intensity(float rho, float aberration);

    // 1 over the mean of 1 + aberration (3 rho^4 - 1) clamped at 0 over the unit disc: 1 for aberration from -0.5 up.
    float dof_bokeh_norm(float aberration);

    // The shape's area over the unit disc's, square-rooted: the blades, their roundness and the anamorphic squeeze.
    float dof_area_scale(uint32_t blades, float roundness, float anamorphic);

    // The farthest the shape reaches from its center, in units of the radius, after the area scale: the gather's radius
    // grows by it.
    float dof_shape_reach(uint32_t blades, float roundness, float anamorphic);
}
