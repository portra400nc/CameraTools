// shaders/dof.hlsli compiled as C++ through hlsl.h. Its constants are this thread's copies of the shaders' cbuffer Dof.
#include "dof.h"
#include "hlsl.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <sstream>

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

        namespace fs = std::filesystem;

        bool write_bytes(const fs::path &path, const void *data, size_t size)
        {
            std::ofstream out(path, std::ios::binary | std::ios::trunc);
            out.write(static_cast<const char *>(data), static_cast<std::streamsize>(size));
            return static_cast<bool>(out);
        }

        template <class T>
        bool read_vector(const fs::path &path, size_t count, std::vector<T> &out)
        {
            std::ifstream in(path, std::ios::binary);
            const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            if (bytes.size() != count * sizeof(T))
                return false;
            out.resize(count);
            std::memcpy(out.data(), bytes.data(), bytes.size());
            return true;
        }

        std::string number(double v)
        {
            char text[40];
            std::snprintf(text, sizeof text, "%.9g", v + 0.0);
            return text;
        }

        std::string list(const float *values, size_t count)
        {
            std::string out;
            for (size_t i = 0; i < count; i++)
                out += (i == 0 ? "" : ",") + number(values[i]);
            return out;
        }
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

    std::optional<std::array<float, 4>> dof_depth_coefficients(const Camera &camera)
    {
        float m[16];
        if (!invert(camera.view_to_clip, m))
            return std::nullopt;
        // Column-major: view z and w of the clip point (0, 0, d, 1) are m[10] d + m[14] and m[11] d + m[15].
        return std::array<float, 4> { m[11], m[15], m[10], m[14] };
    }

    float dof_view_depth(const std::array<float, 4> &k, float reversed)
    {
        const float inverse = shader::dof_inverse_depth(reversed, { k[0], k[1], k[2], k[3] });
        return inverse > 0 ? 1 / inverse : INFINITY;
    }

    std::string frame_tsv(const FrameSave &s)
    {
        std::string out;
        const auto row = [&out](const char *key, const std::string &value) { out += std::string(key) + "\t" + value + "\n"; };
        row("width", std::to_string(s.size.width));
        row("height", std::to_string(s.size.height));
        row("flip", s.flip ? "1" : "0");
        row("frame", std::to_string(s.frame));
        row("world_to_view", list(s.camera.world_to_view, 16));
        row("view_to_clip", list(s.camera.view_to_clip, 16));
        const std::optional<std::array<float, 4>> k = dof_depth_coefficients(s.camera);
        row("near", k ? number(dof_view_depth(*k, 1)) : "-");
        row("far", k ? number(dof_view_depth(*k, 0)) : "-");
        return out;
    }

    std::vector<std::string> write_frame_folder(const fs::path &folder, const FrameSave &s)
    {
        std::error_code error;
        fs::create_directories(folder, error);
        std::vector<std::string> failed;
        if (!write_bytes(folder / "hdr.r11g11b10", s.hdr.data(), s.hdr.size() * sizeof s.hdr[0]))
            failed.push_back("hdr.r11g11b10");
        if (!write_bytes(folder / "depth.r32", s.depth.data(), s.depth.size() * sizeof s.depth[0]))
            failed.push_back("depth.r32");
        const std::string tsv = frame_tsv(s);
        if (!write_bytes(folder / "frame.tsv", tsv.data(), tsv.size()))
            failed.push_back("frame.tsv");
        return failed;
    }

    std::optional<FrameSave> read_frame_folder(const fs::path &folder, std::string &why)
    {
        std::ifstream tsv(folder / "frame.tsv");
        std::map<std::string, std::string> values;
        for (std::string line; std::getline(tsv, line);)
        {
            const size_t tab = line.find('\t');
            if (tab != std::string::npos)
                values[line.substr(0, tab)] = line.substr(tab + 1);
        }
        const auto get = [&values](const char *key) { const auto it = values.find(key); return it == values.end() ? std::string() : it->second; };
        FrameSave s;
        try
        {
            s.size = { static_cast<uint32_t>(std::stoul(get("width"))), static_cast<uint32_t>(std::stoul(get("height"))) };
            s.frame = std::stoull(get("frame"));
        }
        catch (const std::exception &)
        {
            why = "frame.tsv is missing or lacks width, height or frame";
            return std::nullopt;
        }
        s.flip = get("flip") != "0";
        for (const auto &[key, matrix] : { std::pair { "world_to_view", s.camera.world_to_view }, std::pair { "view_to_clip", s.camera.view_to_clip } })
        {
            std::istringstream text(get(key));
            std::string item;
            for (int k = 0; k < 16 && std::getline(text, item, ','); k++)
                matrix[k] = std::strtof(item.c_str(), nullptr);
        }
        const size_t pixels = size_t(s.size.width) * s.size.height;
        if (!read_vector(folder / "hdr.r11g11b10", pixels, s.hdr) || !read_vector(folder / "depth.r32", pixels, s.depth))
        {
            why = "hdr.r11g11b10 or depth.r32 is missing or not " + std::to_string(pixels) + " pixels";
            return std::nullopt;
        }
        return s;
    }
}
