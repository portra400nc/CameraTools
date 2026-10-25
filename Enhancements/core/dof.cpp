// shaders/dof.hlsli compiled as C++ through hlsl.h. Its constants are this thread's copies of the shaders' cbuffer Dof.
#include "dof.h"
#include "hlsl.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <sstream>
#include <thread>

namespace enhancements::shader
{
    thread_local float4 dof_optics, dof_focus, dof_depth, dof_shape, dof_lens, dof_bokeh, dof_sampling, dof_highlight, dof_contrast, dof_display, dof_eye;
    thread_local uint4 dof_sizes, dof_counts;

    // The images the passes read on this thread, as the shaders' textures hold them.
    struct DofTextures
    {
        const enhancements::DofImage *scene = nullptr;
        float2 focus;   // the autofocus's: meters and the frame's mean luminance
        std::vector<float4> prefilter;  // the prefilter's level 0 before the sprites took their light out
        // The prefilter's levels: each rgb and the green CoC, row by row.
        std::vector<std::vector<float4>> source;
        std::vector<uint2> source_sizes;
        std::vector<float2> tiles, dilated;
        std::vector<float4> far, near, near_layer;
        std::vector<float2> cover;
        // The lifted source's levels (dof_lift), and the gather's far field's, which the post-filter reads.
        std::vector<std::vector<float4>> lifted, far_levels;
        std::vector<uint2> lifted_sizes, far_sizes;
        // The display place's ends: the back buffer's values, rgb a pixel and alpha 1, and the main depth as stored, at
        // dof_display's size; then dof_display_in's color and the composite's, at the back buffer's.
        const std::vector<float> *back = nullptr, *main_depth = nullptr;
        std::vector<float4> display;
        std::vector<float> blurred;
    };
    thread_local const DofTextures *textures = nullptr;

    namespace
    {
        int clamp_index(int v, uint size) { return v < 0 ? 0 : v >= int(size) ? int(size) - 1 : v; }
        size_t at(int2 p, uint2 size) { return size_t(clamp_index(p.y, size.y)) * size.x + size_t(clamp_index(p.x, size.x)); }
        uint2 render_size() { return { dof_counts.y, dof_counts.z }; }
        uint2 half_size() { return { dof_sizes.x, dof_sizes.y }; }
        uint2 tile_size() { return { dof_sizes.z, dof_sizes.w }; }
    }

    float dof_load_depth(int2 p) { return textures->scene->depth[at(p, render_size())]; }
    float3 dof_load_scene(int2 p)
    {
        const float *c = &textures->scene->rgb[at(p, render_size()) * 3];
        return { c[0], c[1], c[2] };
    }
    // SampleLevel at level 0 with a linear sampler that clamps.
    float3 dof_sample_scene(float2 position)
    {
        const float x = position.x - 0.5f, y = position.y - 0.5f;
        const int x0 = int(floor(x)), y0 = int(floor(y));
        const float fx = x - floor(x), fy = y - floor(y);
        float3 c;
        for (int j = 0; j < 2; j++)
            for (int i = 0; i < 2; i++)
                c = c + dof_load_scene({ x0 + i, y0 + j }) * ((i == 0 ? 1 - fx : fx) * (j == 0 ? 1 - fy : fy));
        return c;
    }
    float2 dof_load_focus() { return textures->focus; }
    float4 dof_load_prefilter(int2 p) { return textures->prefilter[at(p, half_size())]; }
    float4 dof_load_source(int2 p) { return textures->source[0][at(p, half_size())]; }
    namespace
    {
        // SampleLevel with a linear sampler that clamps: bilinear in the two levels around lod, at a position in level 0's
        // pixels.
        float4 sample_levels(const std::vector<std::vector<float4>> &levels, const std::vector<uint2> &sizes, float2 position, float lod)
        {
            const int top = int(levels.size()) - 1;
            const float l = clamp(lod, 0.0f, float(top));
            const int l0 = int(floor(l)), l1 = min(l0 + 1, top);
            const uint2 base = sizes[0];
            const auto bilinear = [&position, base](const std::vector<float4> &level, uint2 size) {
                const float x = position.x / float(base.x) * float(size.x) - 0.5f, y = position.y / float(base.y) * float(size.y) - 0.5f;
                const int x0 = int(floor(x)), y0 = int(floor(y));
                const float fx = x - floor(x), fy = y - floor(y);
                float4 c;
                for (int j = 0; j < 2; j++)
                    for (int i = 0; i < 2; i++)
                        c = c + level[at({ x0 + i, y0 + j }, size)] * ((i == 0 ? 1 - fx : fx) * (j == 0 ? 1 - fy : fy));
                return c;
            };
            const float t = l - float(l0);
            return bilinear(levels[l0], sizes[l0]) * (1 - t) + bilinear(levels[l1], sizes[l1]) * t;
        }
    }

    float3 dof_sample_source(float2 position, float lod)
    {
        const float4 c = sample_levels(textures->source, textures->source_sizes, position, lod);
        return { c.x, c.y, c.z };
    }
    float dof_sample_coc(float2 position, float lod) { return sample_levels(textures->source, textures->source_sizes, position, lod).w; }
    float4 dof_sample_lifted(float2 position, float lod) { return sample_levels(textures->lifted, textures->lifted_sizes, position, lod); }
    float2 dof_load_tile(int2 t) { return textures->tiles[at(t, tile_size())]; }
    float2 dof_load_dilated(int2 t) { return textures->dilated[at(t, tile_size())]; }
    float4 dof_load_far(int2 p) { return textures->far[at(p, half_size())]; }
    float4 dof_load_near(int2 p) { return textures->near[at(p, half_size())]; }
    float2 dof_load_cover(int2 p) { return textures->cover[at(p, half_size())]; }
    float4 dof_load_near_layer(int2 p) { return textures->near_layer[at(p, half_size())]; }
    float4 dof_sample_far(float2 position, float lod) { return sample_levels(textures->far_levels, textures->far_sizes, position, lod); }

    float4 dof_load_back(int2 p)
    {
        const float *c = &(*textures->back)[at(p, render_size()) * 3];
        return { c[0], c[1], c[2], 1.0f };
    }
    float dof_load_main_depth(int2 p) { return (*textures->main_depth)[at(p, { uint(dof_display.x), uint(dof_display.y) })]; }
    float4 dof_load_display(int2 p) { return textures->display[at(p, render_size())]; }
    float3 dof_load_blurred(int2 p)
    {
        const float *c = &textures->blurred[at(p, render_size()) * 3];
        return { c[0], c[1], c[2] };
    }

#define DOF_FOCUS
#define DOF_PREFILTER
#define DOF_SCATTER
#define DOF_LIFT
#define DOF_TILES
#define DOF_DILATE
#define DOF_GATHER
#define DOF_POST
#define DOF_SPRITE
#define DOF_NEAR
#define DOF_COMPOSITE
#define LOOP
#define UNROLL
#include "../shaders/dof.hlsli"
#define DOF_DISPLAY_IN
#define DOF_DISPLAY_OUT
#include "../shaders/dof_display.hlsli"
#undef UNROLL
#undef LOOP
    static_assert(2 * kDofEyeHalfGrid + 1 == int(enhancements::kDofEyeGrid) && kDofEyeGrid * kDofEyeGrid == 81);

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

    std::array<float, 3> dof_capped_cocs(float depth, float focus, const DofOptics &optics)
    {
        const shader::DofLens lens = shader::lens_of(focus, optics);
        const shader::float3 c = shader::dof_capped_coc(shader::dof_coc(1 / depth, focus, lens), lens);
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

    float dof_source_weight(float r, float d, float aberration) { return shader::dof_channel_weight(r, d, aberration, dof_bokeh_norm(aberration)); }

    float dof_grow_highlight(float luminance, float contrast, float threshold, float gain, float radius)
    {
        return shader::dof_grow_highlight(luminance, contrast, { threshold, gain, shader::kDofHighlightDisc * threshold }, radius);
    }

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

    uint32_t dof_rings(uint32_t samples)
    {
        return std::clamp<uint32_t>(static_cast<uint32_t>(std::lround((std::sqrt(double(samples)) - 1) / 2)), 2, 16);
    }

    std::optional<DofConstants> dof_constants(const DepthOfFieldSettings &s, const Camera &camera, Size render, bool flip, float seconds, uint64_t frame)
    {
        const std::optional<std::array<float, 4>> k = dof_depth_coefficients(camera);
        const float p11 = camera.view_to_clip[5];
        if (!k || !(p11 > 0) || render.width == 0 || render.height == 0)
            return std::nullopt;
        DofConstants c {};
        const uint32_t half_w = (render.width + 1) / 2, half_h = (render.height + 1) / 2;
        const float max_radius = s.max_blur * render.height * 0.5f;
        const float margin = dof_shape_reach(s.blades, s.blade_roundness, s.anamorphic);
        const float step = s.focus_seconds <= 0 ? 1 : 1 - std::pow(0.01f, std::max(seconds, 0.0f) / s.focus_seconds);
        const float c_optics[4] = { s.sensor_height_mm * 0.5f * p11, s.f_number, render.height * 0.5f / s.sensor_height_mm, max_radius };
        const float c_focus[4] = { s.focus_distance, s.focus_point[0], flip ? 1 - s.focus_point[1] : s.focus_point[1], step };
        const float c_shape[4] = { static_cast<float>(s.blades), s.blade_roundness, s.blade_rotation * 3.14159265f / 180,
            dof_area_scale(s.blades, s.blade_roundness, s.anamorphic) };
        const float c_lens[4] = { s.anamorphic, s.cat_eye, s.swirl, s.chromatic };
        const float c_bokeh[4] = { s.spherical_aberration, dof_bokeh_norm(s.spherical_aberration), dof_bokeh_norm(-s.spherical_aberration), margin };
        // The golden ratio's turns spread the frames' patterns evenly, so an accumulated shot averages them.
        const float c_sampling[4] = { static_cast<float>(dof_rings(s.samples)), static_cast<float>(std::fmod(double(frame) * 0.6180339887, 1.0)),
            flip ? 1.0f : -1.0f, float(render.width) / float(render.height) };
        std::copy(c_optics, c_optics + 4, c.optics);
        std::copy(c_focus, c_focus + 4, c.focus);
        std::copy(k->begin(), k->end(), c.depth);
        std::copy(c_shape, c_shape + 4, c.shape);
        std::copy(c_lens, c_lens + 4, c.lens);
        std::copy(c_bokeh, c_bokeh + 4, c.bokeh);
        std::copy(c_sampling, c_sampling + 4, c.sampling);
        c.highlight[0] = s.highlight_threshold;
        c.highlight[1] = s.highlight_gain;
        // As many sprites as kDofSpriteOverdraw half-size pictures of the largest quad.
        const float quad = 2 * (max_radius * margin + 1);
        c.highlight[2] = s.scatter ? std::floor(std::min(float(kDofSpriteCap), float(kDofSpriteOverdraw) * half_w * half_h / (quad * quad))) : 0;
        c.highlight[3] = float(kDofSpriteCap);
        c.contrast[0] = s.bokeh_contrast > 0 ? kDofContrastPower : 0;
        c.contrast[1] = s.bokeh_contrast * kDofContrastShare;
        const uint32_t sizes[4] = { half_w, half_h, (half_w + kDofTile - 1) / kDofTile, (half_h + kDofTile - 1) / kDofTile };
        const uint32_t counts[4] = { static_cast<uint32_t>(std::ceil(max_radius * margin / kDofTile)), render.width, render.height,
            std::max(1u, render.height / 100) };
        std::copy(sizes, sizes + 4, c.sizes);
        std::copy(counts, counts + 4, c.counts);
        if (s.eye_distance > 0)
        {
            // The eye's width on the picture: its meters over its distance, in the projection's pixels per unit of tangent.
            const float window = std::clamp(s.eye_size / s.eye_distance * p11 * render.height * 0.5f, kDofEyeLeastWindow,
                std::max(kDofEyeLeastWindow, kDofEyeWindowShare * render.height));
            c.eye[0] = s.eye_distance;
            c.eye[1] = window / (kDofEyeGrid - 1);
        }
        return c;
    }

    namespace
    {
        // A value stored in an R16F channel and read back: rounded to the nearest of half precision's 11 significant
        // bits, ties to even, and past 65504 infinite, as a GPU's R16F render target holds it.
        float half_stored(float v)
        {
            if (!std::isfinite(v))
                return v;
            const float a = std::fabs(v);
            if (a >= 65520.0f)
                return std::copysign(INFINITY, v);
            if (a < 6.103515625e-05f)
                return std::copysign(std::nearbyint(a / 5.9604644775390625e-08f) * 5.9604644775390625e-08f, v);
            int e;
            std::frexp(a, &e);
            const float ulp = std::ldexp(1.0f, e - 11);
            return std::copysign(std::nearbyint(a / ulp) * ulp, v);
        }

        // Every channel of every texel of a half-float target as stored; returns how many are not finite after.
        uint32_t store_half(std::vector<shader::float4> &texels)
        {
            uint32_t bad = 0;
            for (shader::float4 &t : texels)
                for (float *c : { &t.x, &t.y, &t.z, &t.w })
                {
                    *c = half_stored(*c);
                    bad += std::isfinite(*c) ? 0 : 1;
                }
            return bad;
        }

        uint32_t store_half(std::vector<shader::float2> &texels)
        {
            uint32_t bad = 0;
            for (shader::float2 &t : texels)
                for (float *c : { &t.x, &t.y })
                {
                    *c = half_stored(*c);
                    bad += std::isfinite(*c) ? 0 : 1;
                }
            return bad;
        }

        // GenerateMips over levels, which holds level 0: each level halves, rounded down, each texel the mean of the 2x2 below
        // it, stored as R16F. Returns how many channels are not finite.
        uint32_t generate_mips(std::vector<std::vector<shader::float4>> &levels, std::vector<shader::uint2> &sizes)
        {
            uint32_t bad = 0;
            while (sizes.back().x > 1 || sizes.back().y > 1)
            {
                const shader::uint2 below = sizes.back(), size = { std::max(1u, below.x / 2), std::max(1u, below.y / 2) };
                const std::vector<shader::float4> &from = levels.back();
                std::vector<shader::float4> level(size_t(size.x) * size.y);
                for (uint32_t y = 0; y < size.y; y++)
                    for (uint32_t x = 0; x < size.x; x++)
                    {
                        shader::float4 sum;
                        for (uint32_t j = 0; j < 2; j++)
                            for (uint32_t i = 0; i < 2; i++)
                                sum = sum + from[size_t(std::min(y * 2 + j, below.y - 1)) * below.x + std::min(x * 2 + i, below.x - 1)];
                        level[size_t(y) * size.x + x] = sum * 0.25f;
                    }
                bad += store_half(level);
                levels.push_back(std::move(level));
                sizes.push_back(size);
            }
            return bad;
        }

        void set_constants(const DofConstants &c)
        {
            const auto f4 = [](const float *v) { return shader::float4(v[0], v[1], v[2], v[3]); };
            shader::dof_optics = f4(c.optics);
            shader::dof_focus = f4(c.focus);
            shader::dof_depth = f4(c.depth);
            shader::dof_shape = f4(c.shape);
            shader::dof_lens = f4(c.lens);
            shader::dof_bokeh = f4(c.bokeh);
            shader::dof_sampling = f4(c.sampling);
            shader::dof_highlight = f4(c.highlight);
            shader::dof_contrast = f4(c.contrast);
            shader::dof_display = f4(c.display);
            shader::dof_eye = f4(c.eye);
            shader::dof_sizes = { c.sizes[0], c.sizes[1], c.sizes[2], c.sizes[3] };
            shader::dof_counts = { c.counts[0], c.counts[1], c.counts[2], c.counts[3] };
        }

        // Runs pixel(x, y) over a width x height target on the machine's threads, each with the constants and textures set,
        // as a draw runs its pixel shader.
        template <class Pixel>
        void draw(uint32_t width, uint32_t height, const DofConstants &c, const shader::DofTextures &t, Pixel pixel)
        {
            std::atomic<uint32_t> next { 0 };
            const auto work = [&] {
                set_constants(c);
                shader::textures = &t;
                for (uint32_t y; (y = next++) < height;)
                    for (uint32_t x = 0; x < width; x++)
                        pixel(x, y);
            };
            const unsigned count = std::clamp(std::thread::hardware_concurrency(), 1u, 16u);
            std::vector<std::thread> threads;
            for (unsigned i = 1; i < count; i++)
                threads.emplace_back(work);
            work();
            for (std::thread &thread : threads)
                thread.join();
        }
    }

    DofOutput render_dof(const DofImage &image, const DofConstants &c, float previous_focus)
    {
        using namespace shader;
        DofTextures t;
        t.scene = &image;
        set_constants(c);
        textures = &t;
        t.focus = dof_focus_pass(previous_focus);
        const uint2 half = { c.sizes[0], c.sizes[1] }, tiles = { c.sizes[2], c.sizes[3] };
        t.source.emplace_back(size_t(half.x) * half.y);
        t.source_sizes.push_back(half);
        draw(half.x, half.y, c, t, [&t, half](uint32_t x, uint32_t y) { t.source[0][size_t(y) * half.x + x] = dof_prefilter(int2(int(x), int(y))); });
        uint32_t nonfinite = store_half(t.source[0]);
        // dof_scatter_cs.hlsl: each highlight takes the next slot of its field's buffer while one is free, and its light out
        // of the gather's source.
        const uint32_t cap = static_cast<uint32_t>(c.highlight[2]);
        std::vector<DofSprite> sprites[2];
        if (cap > 0)
        {
            t.prefilter = t.source[0];
            std::vector<DofDetect> found(t.prefilter.size());
            draw(half.x, half.y, c, t, [&found, half](uint32_t x, uint32_t y) { found[size_t(y) * half.x + x] = dof_detect(int2(int(x), int(y))); });
            for (size_t i = 0; i < found.size(); i++)
            {
                std::vector<DofSprite> &field = sprites[found[i].as.at.z > 0 ? 0 : 1];
                if (!found[i].sprite || field.size() >= cap)
                    continue;
                field.push_back(found[i].as);
                t.source[0][i] = found[i].rest;
            }
            nonfinite += store_half(t.source[0]);
        }
        nonfinite += generate_mips(t.source, t.source_sizes);
        // dof_lift_ps.hlsl and its mips, with the bokeh contrast on.
        if (c.contrast[0] > 0)
        {
            t.lifted = { std::vector<float4>(t.source[0].size()) };
            t.lifted_sizes = { half };
            draw(half.x, half.y, c, t, [&t, half](uint32_t x, uint32_t y) { t.lifted[0][size_t(y) * half.x + x] = dof_lift(int2(int(x), int(y))); });
            nonfinite += store_half(t.lifted[0]) + generate_mips(t.lifted, t.lifted_sizes);
        }
        t.tiles.resize(size_t(tiles.x) * tiles.y);
        draw(tiles.x, tiles.y, c, t, [&t, tiles](uint32_t x, uint32_t y) { t.tiles[size_t(y) * tiles.x + x] = dof_tile(int2(int(x), int(y))); });
        nonfinite += store_half(t.tiles);
        t.dilated.resize(t.tiles.size());
        draw(tiles.x, tiles.y, c, t, [&t, tiles](uint32_t x, uint32_t y) { t.dilated[size_t(y) * tiles.x + x] = dof_dilate(int2(int(x), int(y))); });
        nonfinite += store_half(t.dilated);
        std::vector<float4> far(size_t(half.x) * half.y), near(far.size());
        std::vector<float2> cover(far.size());
        draw(half.x, half.y, c, t, [&](uint32_t x, uint32_t y) {
            const DofFields f = dof_gather(int2(int(x), int(y)));
            far[size_t(y) * half.x + x] = f.far;
            near[size_t(y) * half.x + x] = f.near;
            cover[size_t(y) * half.x + x] = f.cover;
        });
        t.far = std::move(far);
        t.near = std::move(near);
        t.cover = std::move(cover);
        nonfinite += store_half(t.far) + store_half(t.near) + store_half(t.cover);
        if (c.contrast[0] > 0)
        {
            t.far_levels = { t.far };
            t.far_sizes = { half };
            nonfinite += generate_mips(t.far_levels, t.far_sizes);
        }
        std::vector<float4> far_post(t.far.size()), near_post(t.far.size());
        std::vector<float2> cover_post(t.far.size());
        draw(half.x, half.y, c, t, [&](uint32_t x, uint32_t y) {
            const DofFields f = dof_post(int2(int(x), int(y)));
            far_post[size_t(y) * half.x + x] = f.far;
            near_post[size_t(y) * half.x + x] = f.near;
            cover_post[size_t(y) * half.x + x] = f.cover;
        });
        t.far = std::move(far_post);
        t.near = std::move(near_post);
        t.cover = std::move(cover_post);
        nonfinite += store_half(t.far) + store_half(t.near) + store_half(t.cover);
        // dof_sprite_vs.hlsl and dof_sprite_ps.hlsl: each sprite's quad added into its field, the pixels whose centers it
        // covers.
        for (int f = 0; f < 2; f++)
        {
            std::vector<std::vector<uint32_t>> rows(half.y);
            for (uint32_t i = 0; i < sprites[f].size(); i++)
            {
                const float4 a = sprites[f][i].at;
                for (int y = std::max(0, int(std::ceil(a.y - a.w - 0.5f))); y < int(half.y) && float(y) + 0.5f <= a.y + a.w; y++)
                    rows[size_t(y)].push_back(i);
            }
            std::vector<float4> &field = f == 0 ? t.far : t.near;
            std::vector<float4> added(field.size());
            draw(half.x, half.y, c, t, [&](uint32_t x, uint32_t y) {
                float3 sum;
                for (const uint32_t i : rows[y])
                {
                    const float4 a = sprites[f][i].at;
                    if (std::fabs(float(x) + 0.5f - a.x) <= a.w)
                        sum = sum + dof_sprite_light(sprites[f][i], int2(int(x), int(y)));
                }
                added[size_t(y) * half.x + x] = float4(sum.x, sum.y, sum.z, 0.0f);
            });
            for (size_t i = 0; i < field.size(); i++)
                field[i] = field[i] + added[i];
            nonfinite += store_half(field);
        }
        // dof_near_ps.hlsl: the near plane over the post-filtered far field with its sprites.
        std::vector<float4> layer(t.far.size());
        draw(half.x, half.y, c, t, [&](uint32_t x, uint32_t y) { layer[size_t(y) * half.x + x] = dof_near_layer(int2(int(x), int(y))); });
        t.near_layer = std::move(layer);
        nonfinite += store_half(t.near_layer);
        DofOutput out;
        out.sprites[0] = static_cast<uint32_t>(sprites[0].size());
        out.sprites[1] = static_cast<uint32_t>(sprites[1].size());
        out.nonfinite = nonfinite;
        out.focus = t.focus.x;
        out.luminance = t.focus.y;
        out.rgb.resize(image.rgb.size());
        const uint32_t width = image.size.width;
        draw(width, image.size.height, c, t, [&out, width](uint32_t x, uint32_t y) {
            const float3 v = dof_composite(int2(int(x), int(y)));
            float *o = &out.rgb[(size_t(y) * width + x) * 3];
            o[0] = v.x;
            o[1] = v.y;
            o[2] = v.z;
        });
        for (const float v : out.rgb)
            out.nonfinite += std::isfinite(v) ? 0 : 1;
        textures = nullptr;
        return out;
    }

    DofAtBloom dof_at_bloom(DofPlace place, AccumulateMode accumulate)
    {
        if (place == DofPlace::scene)
            return DofAtBloom::blur;
        return accumulate == AccumulateMode::add ? DofAtBloom::focus : DofAtBloom::defer;
    }

    DofEffectsPoint dof_effects_point(const std::vector<bool> &reads_depth)
    {
        for (size_t i = reads_depth.size(); i-- > 0;)
            if (reads_depth[i])
                return { i };
        return {};
    }

    bool dof_runs_after(const DofEffectsPoint &point, size_t rendered) { return point.after && rendered >= *point.after; }

    std::optional<DofTransfer> dof_display_transfer(uint32_t format)
    {
        switch (format)
        {
        case 23: case 24:                   // R10G10B10A2_TYPELESS, _UNORM
        case 27: case 28: case 29:          // R8G8B8A8_TYPELESS, _UNORM, _UNORM_SRGB
        case 87: case 88: case 90: case 91: case 92: case 93:  // B8G8R8A8_UNORM, B8G8R8X8_UNORM, their TYPELESS and _UNORM_SRGB
            return DofTransfer::srgb;
        case 9: case 10:                    // R16G16B16A16_TYPELESS, _FLOAT: scRGB
            return DofTransfer::linear;
        default:
            return std::nullopt;
        }
    }

    bool dof_depth_format(uint32_t format)
    {
        switch (format)
        {
        case 19: case 20: case 21: case 22:     // R32G8X24_TYPELESS, D32_FLOAT_S8X24_UINT and its two views
        case 44: case 45: case 46: case 47:     // R24G8_TYPELESS, D24_UNORM_S8_UINT and its two views
        case 40: case 55:                       // D32_FLOAT, D16_UNORM
            return true;
        default:
            return false;
        }
    }

    float dof_srgb_decode(float v) { return shader::dof_srgb_to_linear(v); }
    float dof_srgb_encode(float c) { return shader::dof_linear_to_srgb(c); }

    std::optional<DofConstants> dof_display_constants(const DepthOfFieldSettings &settings, const Camera &camera, Size back, Size main, bool main_flip,
        DofTransfer transfer, uint64_t frame)
    {
        std::optional<DofConstants> c = dof_constants(settings, camera, back, false, 0, frame);
        if (!c || main.width == 0 || main.height == 0)
            return std::nullopt;
        c->focus[3] = 0;
        c->display[0] = float(main.width);
        c->display[1] = float(main.height);
        c->display[2] = main_flip ? 1.0f : 0.0f;
        c->display[3] = transfer == DofTransfer::srgb ? 1.0f : 0.0f;
        return c;
    }

    DofOutput render_dof_display(const std::vector<float> &back, const std::vector<float> &main_depth, const DofConstants &c, float previous_focus)
    {
        using namespace shader;
        DofTextures t;
        t.back = &back;
        t.main_depth = &main_depth;
        const uint32_t width = c.counts[1], height = c.counts[2];
        DofImage image;
        image.size = { width, height };
        image.depth.resize(size_t(width) * height);
        t.display.resize(image.depth.size());
        draw(width, height, c, t, [&](uint32_t x, uint32_t y) {
            const DofDisplayIn d = dof_display_in(int2(int(x), int(y)));
            t.display[size_t(y) * width + x] = d.color;
            image.depth[size_t(y) * width + x] = d.depth;
        });
        uint32_t nonfinite = store_half(t.display);
        image.rgb.resize(image.depth.size() * 3);
        for (size_t i = 0; i < t.display.size(); i++)
        {
            image.rgb[i * 3] = t.display[i].x;
            image.rgb[i * 3 + 1] = t.display[i].y;
            image.rgb[i * 3 + 2] = t.display[i].z;
        }
        DofOutput out = render_dof(image, c, previous_focus);
        for (float &v : out.rgb)
            v = half_stored(v);
        t.blurred = std::move(out.rgb);
        out.rgb.assign(t.blurred.size(), 0.0f);
        draw(width, height, c, t, [&](uint32_t x, uint32_t y) {
            const float4 v = dof_display_out(int2(int(x), int(y)));
            float *o = &out.rgb[(size_t(y) * width + x) * 3];
            o[0] = v.x;
            o[1] = v.y;
            o[2] = v.z;
        });
        out.nonfinite += nonfinite;
        return out;
    }
}
