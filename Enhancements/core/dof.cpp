// shaders/dof.hlsli compiled as C++ through hlsl.h. Its constants are this thread's copies of the shaders' cbuffer Dof.
#include "dof.h"
#include "hlsl.h"

#include <algorithm>
#include <cmath>

namespace enhancements::shader
{
    thread_local float4 dof_optics, dof_focus, dof_depth, dof_shape, dof_lens, dof_bokeh, dof_sampling;
    thread_local uint4 dof_sizes, dof_counts;

#define LOOP
#define UNROLL
#include "../shaders/cat_eye.hlsli"
#include "../shaders/dof.hlsli"
#undef UNROLL
#undef LOOP

    namespace
    {
        DofLens lens_of(float focus, const enhancements::DofOptics &o)
        {
            return dof_lens_at(o.focal_mm, o.f_number, focus, o.pixels_per_mm, o.chromatic, o.max_radius);
        }

        DofShape shape_of(const enhancements::DofShapeParams &p)
        {
            DofShape s;
            s.blades = static_cast<float>(p.blades);
            s.roundness = p.roundness;
            s.rotation = p.rotation_degrees * kDofPi / 180.0f;
            s.area_scale = enhancements::dof_area_scale(p.blades, p.roundness, p.anamorphic);
            s.anamorphic = p.anamorphic;
            s.cat_eye = p.cat_eye;
            s.swirl = p.swirl;
            s.aspect = p.aspect;
            return s;
        }
    }
}

namespace enhancements
{
    namespace
    {
        // The shape's edge at angle theta, in the squeezed coordinates the polygon is drawn in, before the area scale.
        double edge_radius(double theta, uint32_t blades, float roundness)
        {
            if (blades < 3)
                return 1;
            const double sector = 2 * 3.14159265358979 / blades;
            const double t = theta - 3.14159265358979 * 0.5 - sector * 0.5;
            const double phi = t - sector * std::floor(t / sector + 0.5);
            return 1 / (std::cos(phi) / std::cos(sector * 0.5) * (1 - roundness) + roundness);
        }

        constexpr int kEdgeSteps = 4096;
    }

    std::array<float, 3> dof_channel_cocs(float depth, float focus, const DofOptics &optics)
    {
        const shader::DofLens lens = shader::lens_of(focus, optics);
        const shader::float3 c = shader::dof_channel_coc(shader::dof_coc(1 / depth, focus, lens), lens);
        return { c.x, c.y, c.z };
    }

    float dof_green_coc(float depth, float focus, const DofOptics &optics)
    {
        return shader::dof_coc(1 / depth, focus, shader::lens_of(focus, optics));
    }

    float dof_aperture(float ux, float uy, float r, float ndc_x, float ndc_y, const DofShapeParams &shape)
    {
        return shader::dof_aperture({ ux, uy }, r, { ndc_x, ndc_y }, shader::shape_of(shape));
    }

    float dof_bokeh_intensity(float rho, float aberration) { return shader::dof_bokeh_intensity(rho, aberration, dof_bokeh_norm(aberration)); }

    float dof_bokeh_norm(float aberration)
    {
        if (aberration >= -0.5f)
            return 1;
        const double a = aberration;
        const double r2 = std::sqrt((a - 1) / (3 * a));
        return static_cast<float>(1 / ((1 - a) * r2 + a * r2 * r2 * r2));
    }

    float dof_area_scale(uint32_t blades, float roundness, float anamorphic)
    {
        double area = 0;
        for (int i = 0; i < kEdgeSteps; i++)
        {
            const double r = edge_radius(2 * 3.14159265358979 * (i + 0.5) / kEdgeSteps, blades, roundness);
            area += 0.5 * r * r * 2 * 3.14159265358979 / kEdgeSteps;
        }
        return static_cast<float>(std::sqrt(area / anamorphic / 3.14159265358979));
    }

    float dof_shape_reach(uint32_t blades, float roundness, float anamorphic)
    {
        double reach = 0;
        for (int i = 0; i < kEdgeSteps; i++)
        {
            const double theta = 2 * 3.14159265358979 * (i + 0.5) / kEdgeSteps;
            const double r = edge_radius(theta, blades, roundness);
            reach = std::max(reach, std::hypot(r * std::cos(theta) / anamorphic, r * std::sin(theta)));
        }
        return static_cast<float>(reach / dof_area_scale(blades, roundness, anamorphic));
    }
}
